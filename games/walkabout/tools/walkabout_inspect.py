#!/usr/bin/env python3
"""Read-only, hash-pinned Walkabout 57013 authentication/method inventory.

Metadata v39 layout reference: SamboyCoding/Cpp2IL commit
b5ad444b82267cb1e4b88b8b373c008105bdea52, LibCpp2IL/Metadata.
This tool never executes or edits a game, requests a ticket, or contacts a server.
Supplied binaries, method maps and extracted configuration stay in ignored build/.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[3]
LOCK = ROOT / 'games/walkabout/tools/walkabout_login.lock.json'
SECTIONS = ('stringLiteral stringLiteralData string events properties methods '
    'parameterDefaultValues fieldDefaultValues fieldAndParameterDefaultValueData '
    'fieldMarshaledSizes parameters fields genericParameters genericParameterConstraints '
    'genericContainers nestedTypes interfaces vtableMethods interfaceOffsets '
    'typeDefinitions images assemblies fieldRefs referencedAssemblies attributeData '
    'attributeDataRange unresolvedVirtualCallParameterTypes unresolvedVirtualCallParameterRanges '
    'windowsRuntimeTypeNames windowsRuntimeStrings exportedTypeDefinitions').split()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def region(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('region outside input')
    return data[offset:offset + size]


def unpack(fmt, data, offset=0):
    return struct.unpack(fmt, region(data, offset, struct.calcsize(fmt)))


def pinned(path, record):
    data = path.read_bytes()
    if len(data) != record['size'] or digest(data) != record['sha256']:
        raise ValueError(f'input does not match pinned artifact: {path.name}')
    return data


class Metadata39:
    """Only the v39 tables needed for method/field names and module matching.

    Derive variable index widths from independent table strides, then validate
    all image/type/method ownership and tokens. Do not guess another version.
    """
    def __init__(self, data):
        self.data = data
        if unpack('<II', data) != (0xFAB11BAF, 39):
            raise ValueError('expected IL2CPP metadata version 39')
        self.sections = {}
        header_size = 8 + 12 * len(SECTIONS)
        for i, name in enumerate(SECTIONS):
            off, size, count = unpack('<III', data, 8 + i * 12)
            region(data, off, size)
            if off < header_size or (count and not size):
                raise ValueError('invalid metadata section')
            self.sections[name] = (off, size, count)
        self.type_width = self.stride('fields') - 8
        self.definition_width = (self.stride('images') - 32) // 2
        if 32 + 2 * self.definition_width != self.stride('images'):
            raise ValueError('invalid image index layout')
        self.generic_width = self.stride('typeDefinitions') - 68 - 3 * self.type_width
        self.parameter_width = self.stride('methods') - 20 - self.definition_width - self.type_width - self.generic_width
        if any(w not in (1, 2, 4) for w in (self.type_width, self.definition_width, self.generic_width, self.parameter_width)):
            raise ValueError('unsupported metadata index width')
        self.images, self.types, self.methods = [], [], []
        owner = [None] * self.sections['typeDefinitions'][2]
        for i, b in self.records('images'):
            name = self.string(unpack('<i', b)[0])
            first = self.index(b, 8, self.definition_width)
            count = unpack('<I', b, 8 + self.definition_width)[0]
            if first < 0 or first + count > len(owner):
                raise ValueError('image type range outside table')
            for t in range(first, first + count):
                if owner[t] is not None:
                    raise ValueError('overlapping image type ranges')
                owner[t] = name
            self.images.append({'index': i, 'name': name, 'first': first, 'count': count})
        if any(o is None for o in owner):
            raise ValueError('image ranges do not cover types')
        start = 12 + 3 * self.type_width + self.generic_width
        for i, b in self.records('typeDefinitions'):
            name, ns = unpack('<ii', b)
            ff, fm = unpack('<ii', b, start)
            mc, pc, fc = unpack('<HHH', b, start + 32)
            token = unpack('<I', b, len(b) - 4)[0]
            if token >> 24 != 2:
                raise ValueError('invalid type token')
            for first, count, section in ((fm, mc, 'methods'), (ff, fc, 'fields')):
                if count and (first < 0 or first + count > self.sections[section][2]):
                    raise ValueError('type member range outside table')
            namespace = self.string(ns)
            self.types.append({'index': i, 'name': (namespace + '.' if namespace else '') + self.string(name),
                'image': owner[i], 'first_method': fm, 'method_count': mc, 'first_field': ff, 'field_count': fc})
        token_offset = 8 + self.definition_width + self.type_width + self.parameter_width + self.generic_width
        for i, b in self.records('methods'):
            name = self.string(unpack('<i', b)[0])
            declaring = self.index(b, 4, self.definition_width)
            token = unpack('<I', b, token_offset)[0]
            if not 0 <= declaring < len(self.types) or token >> 24 != 6 or not token & 0xFFFFFF:
                raise ValueError('invalid method token or declaring type')
            t = self.types[declaring]
            self.methods.append({'index': i, 'name': name, 'declaring_type': declaring, 'type': t['name'],
                'image': t['image'], 'token': token, 'parameter_count': unpack('<H', b, len(b)-2)[0]})
        covered = set()
        for t in self.types:
            for m in self.methods[t['first_method']:t['first_method']+t['method_count']]:
                if m['declaring_type'] != t['index'] or m['index'] in covered:
                    raise ValueError('method/type ownership mismatch')
                covered.add(m['index'])
        if len(covered) != len(self.methods):
            raise ValueError('type ranges do not cover methods')
        per_image = defaultdict(list)
        for m in self.methods:
            per_image[m['image']].append(m['token'] & 0xFFFFFF)
        self.method_counts = {}
        for image in self.images:
            tokens = sorted(per_image[image['name']])
            if tokens != list(range(1, len(tokens)+1)):
                raise ValueError('method tokens are not contiguous within image')
            self.method_counts[image['name']] = len(tokens)

    def stride(self, name):
        off, size, count = self.sections[name]
        if not count or size % count:
            raise ValueError('invalid metadata record stride')
        return size // count

    def records(self, name):
        off, size, count = self.sections[name]
        stride = self.stride(name)
        for i in range(count):
            yield i, region(self.data, off + i*stride, stride)

    @staticmethod
    def index(data, off, width):
        v = int.from_bytes(region(data, off, width), 'little')
        return -1 if v == (1 << (8*width))-1 else v

    def string(self, idx):
        off, size, count = self.sections['string']
        if not 0 <= idx < size:
            raise ValueError('metadata string index outside table')
        end = self.data.find(b'\0', off+idx, off+size)
        if end < 0:
            raise ValueError('unterminated metadata string')
        return self.data[off+idx:end].decode('utf-8')

    def literal(self, idx):
        off, size, count = self.sections['stringLiteral']
        data_off, data_size, _ = self.sections['stringLiteralData']
        if size != count * 4 or not 0 <= idx < count-1:
            raise ValueError('invalid literal index/layout')
        lo, hi = unpack('<II', self.data, off + idx*4)
        if lo > hi or hi > data_size:
            raise ValueError('literal outside data section')
        return region(self.data, data_off+lo, hi-lo).decode('utf-8')

    def fields(self, type_name):
        matches = [t for t in self.types if t['name'] == type_name]
        if len(matches) != 1:
            raise ValueError('ambiguous type name')
        t = matches[0]
        off, size, count = self.sections['fields']
        stride = self.stride('fields')
        return [self.string(unpack('<i', self.data, off+i*stride)[0])
            for i in range(t['first_field'], t['first_field']+t['field_count'])]


class Arm64ELF:
    def __init__(self, data):
        self.data = data
        if region(data, 0, 6) != b'\x7fELF\x02\x01' or unpack('<H', data, 18)[0] != 183:
            raise ValueError('expected little-endian ARM64 ELF')
        self.loads, self.relative, self.targets = [], {}, defaultdict(list)
        phoff, shoff = unpack('<QQ', data, 32)
        phsize, phcount, shsize, shcount = unpack('<HHHH', data, 54)
        if phsize != 56 or shsize != 64:
            raise ValueError('unexpected ELF header strides')
        for i in range(phcount):
            kind, flags, off, va, pa, size, mem, align = unpack('<II6Q', data, phoff+i*phsize)
            if kind == 1:
                region(data, off, size)
                self.loads.append((va, off, size, flags))
        for i in range(shcount):
            sh = unpack('<II4QII2Q', data, shoff+i*shsize)
            if sh[1] != 4:
                continue
            if sh[5] % 24 or sh[9] != 24:
                raise ValueError('invalid RELA section')
            region(data, sh[4], sh[5])
            for off in range(sh[4], sh[4]+sh[5], 24):
                address, info, addend = unpack('<QQq', data, off)
                if info & 0xFFFFFFFF == 1027: # R_AARCH64_RELATIVE
                    self.raw(address, 8)
                    if address in self.relative:
                        raise ValueError('duplicate relative relocation')
                    self.relative[address] = addend
                    self.targets[addend].append(address)

    def raw(self, va, size=1):
        matches = [o+va-v for v,o,n,f in self.loads if v <= va and va+size <= v+n]
        if len(matches) != 1:
            raise ValueError('virtual address outside unique file-backed load')
        return matches[0]

    def pointer(self, va):
        if va in self.relative:
            return self.relative[va]
        return unpack('<Q', self.data, self.raw(va, 8))[0]

    def executable(self, va):
        return any(f & 1 and v <= va < v+n for v,o,n,f in self.loads)

    def method_map(self, metadata):
        modules = {}
        for image in metadata.images:
            needle = image['name'].encode() + b'\0'
            names, pos = [], 0
            while (at := self.data.find(needle, pos)) >= 0:
                pos = at+1
                names.extend(v+at-o for v,o,n,f in self.loads if o <= at < o+n)
            expected = metadata.method_counts[image['name']]
            candidates = []
            for name in names:
                for va in self.targets.get(name, []):
                    count = unpack('<Q', self.data, self.raw(va+8, 8))[0]
                    if count != expected:
                        continue
                    try:
                        pointers = self.pointer(va+16)
                        self.raw(pointers, max(1, count*8))
                        values = [self.pointer(pointers+i*8) for i in range(count)]
                        if any(p and not self.executable(p) for p in values):
                            continue
                    except ValueError:
                        continue
                    candidates.append((va, pointers, values))
            if len(candidates) != 1:
                raise ValueError('missing or ambiguous codegen module: '+image['name'])
            va, pointers, values = candidates[0]
            modules[image['name']] = {'address': va, 'method_pointers': pointers, 'count': expected}
            for m in metadata.methods:
                if m['image'] == image['name']:
                    m['address'] = values[(m['token'] & 0xFFFFFF)-1]
        return modules


def verify_settings_script(assets, record):
    parts = sorted(assets.glob('globalgamemanagers.assets.split*'),
                   key=lambda p: int(p.name.rsplit('split', 1)[1]))
    if not parts or [int(p.name.rsplit('split', 1)[1]) for p in parts] != list(range(len(parts))):
        raise ValueError('missing or noncontiguous script asset splits')
    data = b''.join(p.read_bytes() for p in parts)
    if digest(data) != record['script_inventory_sha256']:
        raise ValueError('MonoScript asset inventory hash mismatch')
    obj = region(data, record['script_object_offset'], record['script_object_size'])
    if digest(obj) != record['script_object_sha256']:
        raise ValueError('MonoScript object hash mismatch')


def settings_from_obb(path, record):
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo(record['member'])
        if info.file_size != record['size'] or f'{info.CRC:08x}' != record['crc32']:
            raise ValueError('settings member size/CRC mismatch')
        data = archive.read(info) # zipfile also checks the CRC.
    if digest(data) != record['sha256']:
        raise ValueError('settings member hash mismatch')
    obj = region(data, record['object_offset'], record['object_size'])
    if unpack('<iq', obj, 16) != (1, 311):
        raise ValueError('unexpected settings MonoScript reference')
    name_size = unpack('<I', obj, 28)[0]
    if region(obj, 32, name_size) != b'PlayFabSharedSettings':
        raise ValueError('unexpected settings object name')
    at = (32 + name_size + 3) & ~3
    length = unpack('<I', obj, at)[0]
    title = region(obj, at+4, length).decode('ascii')
    if not title or len(title) > 32 or any(c not in '0123456789ABCDEFabcdef' for c in title):
        raise ValueError('invalid packaged Title ID')
    return title


def inspect(metadata_path, binary_path, lock, obb=None, assets=None):
    meta = Metadata39(pinned(metadata_path, lock['metadata']))
    elf = Arm64ELF(pinned(binary_path, lock['binary']))
    modules = elf.method_map(meta)
    selected = []
    for expected in lock['methods']:
        matches = [m for m in meta.methods if m['image'] == expected['image'] and
            m['type'] == expected['type'] and m['name'] == expected['name'] and m['token'] == expected['token']]
        if len(matches) != 1 or matches[0]['address'] != expected['address']:
            raise ValueError('pinned method mapping mismatch')
        selected.append(matches[0])
    for literal in lock['literals']:
        if meta.literal(literal['index']) != literal['value']:
            raise ValueError('pinned literal mismatch')
    fields = meta.fields('PlayFab.ClientModels.LoginWithSteamRequest')
    if fields != lock['login_request_fields']:
        raise ValueError('Steam request layout changed')
    title = None
    if obb:
        verify_settings_script(assets or metadata_path.parents[2], lock['playfab_settings'])
        title = settings_from_obb(obb, lock['playfab_settings'])
    if title is not None and title != lock['playfab_settings']['title_id']:
        raise ValueError('packaged title mismatch')
    return {'scope': 'read-only static inventory; no login or runtime behavior established',
        'target': lock['target'], 'metadata_sha256': lock['metadata']['sha256'],
        'binary_sha256': lock['binary']['sha256'], 'metadata_version': 39,
        'module_count': len(modules), 'method_count': len(meta.methods),
        'packaged_playfab_title_id': title, 'runtime_playfab_title_id': None,
        'login_request_fields': fields, 'methods': selected,
        'literals': lock['literals'], 'callsite_audit': lock['callsite_audit'],
        'authenticated': False, 'private_room_joined': False, 'voice_verified': False, 'dlc_entitlements_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', type=Path, default=ROOT/'walkabout-57013/assets/bin/Data/Managed/Metadata/global-metadata.dat')
    parser.add_argument('--binary', type=Path, default=ROOT/'walkabout-57013/lib/arm64-v8a/libil2cpp.so')
    parser.add_argument('--obb', type=Path, help='optional supplied main OBB; validate packaged PlayFab settings')
    parser.add_argument('--assets', type=Path, help='Data directory containing script asset splits; inferred from metadata by default')
    parser.add_argument('--out', type=Path, default=ROOT/'build/walkabout-auth-analysis/inspection.json')
    args = parser.parse_args()
    report = inspect(args.metadata, args.binary, json.loads(LOCK.read_text()), args.obb, args.assets)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2)+'\n')
    print(f'Inspected {report["module_count"]} modules and {report["method_count"]} methods; report: {args.out}')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, zipfile.BadZipFile, KeyError) as error:
        raise SystemExit(f'Walkabout inspection failed: {error}')
