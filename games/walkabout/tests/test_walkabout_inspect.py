import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('walkabout_inspect', Path(__file__).resolve().parents[3]/'games/walkabout/tools/walkabout_inspect.py')
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)


def metadata_fixture():
    strings = b'Fixture.dll\0Fixture\0Game\0Method\0Field\0'
    name = lambda s: strings.index(s.encode()+b'\0')
    typ = bytearray(76)
    struct.pack_into('<ii', typ, 0, name('Fixture'), name('Game'))
    struct.pack_into('<ii', typ, 20, 0, 0)
    struct.pack_into('<HHH', typ, 52, 1, 0, 1)
    struct.pack_into('<I', typ, 72, 0x02000001)
    method = struct.pack('<iHHIiHIHHHH', name('Method'), 0, 0, 0, -1, 0xFFFF, 0x06000001, 0, 0, 0, 0)
    image = struct.pack('<iiHIHIiIii', name('Fixture.dll'), 0, 0, 1, 0xFFFF, 0, -1, 1, 0, 0)
    tables = {
        'string': (strings, 5), 'stringLiteral': (struct.pack('<II', 0, 4), 2),
        'stringLiteralData': (b'test', 1), 'typeDefinitions': (bytes(typ), 1),
        'methods': (method, 1), 'images': (image, 1),
        'fields': (struct.pack('<iHI', name('Field'), 0, 0x04000001), 1),
    }
    data = bytearray(8+12*len(w.SECTIONS))
    struct.pack_into('<II', data, 0, 0xFAB11BAF, 39)
    for i, section in enumerate(w.SECTIONS):
        payload, count = tables.get(section, (b'', 0))
        struct.pack_into('<III', data, 8+12*i, len(data), len(payload), count)
        data.extend(payload)
    return data


def elf_fixture():
    d = bytearray(0x900)
    d[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', d, 18, 183)
    struct.pack_into('<QQ', d, 32, 64, 0x800)
    struct.pack_into('<HHHH', d, 54, 56, 2, 64, 2)
    struct.pack_into('<II6Q', d, 64, 1, 5, 0, 0x1000, 0, 0x400, 0x400, 0x1000)
    struct.pack_into('<II6Q', d, 120, 1, 6, 0x400, 0x2000, 0, 0x400, 0x400, 0x1000)
    d[0x500:0x50c] = b'Fixture.dll\0'
    struct.pack_into('<Q', d, 0x608, 1)
    for i, (a, v) in enumerate(((0x2200, 0x2100), (0x2210, 0x2280), (0x2280, 0x1180))):
        struct.pack_into('<QQq', d, 0x700+24*i, a, 1027, v)
    struct.pack_into('<II4QII2Q', d, 0x840, 0, 4, 0, 0, 0x700, 72, 0, 0, 8, 24)
    return d


class MetadataTests(unittest.TestCase):
    def test_v39_ownership_tokens_and_variable_layout(self):
        m = w.Metadata39(metadata_fixture())
        self.assertEqual(m.methods[0]['type'], 'Game.Fixture')
        self.assertEqual(m.method_counts, {'Fixture.dll': 1})
        self.assertEqual(m.fields('Game.Fixture'), ['Field'])
        self.assertEqual(m.literal(0), 'test')
        self.assertEqual((m.type_width, m.definition_width, m.parameter_width), (2, 2, 4))

    def test_bad_version_bounds_stride_and_token_fail(self):
        original = metadata_fixture()
        changes = [(4, '<I', 24), (8, '<I', len(original)+1),
                   (8+12*w.SECTIONS.index('methods')+4, '<I', 29)]
        method_off = struct.unpack_from('<I', original, 8+12*w.SECTIONS.index('methods'))[0]
        changes.append((method_off+18, '<I', 0x04000001))
        for at, fmt, value in changes:
            with self.subTest(at=at):
                d = original.copy(); struct.pack_into(fmt, d, at, value)
                with self.assertRaises(ValueError): w.Metadata39(d)
        with self.assertRaises(ValueError): w.Metadata39(original[:100])

    def test_string_and_literal_bounds_fail(self):
        m = w.Metadata39(metadata_fixture())
        with self.assertRaises(ValueError): m.string(m.sections['string'][1])
        with self.assertRaises(ValueError): m.literal(1) # trailing sentinel isn't a literal
        d = metadata_fixture(); off = m.sections['stringLiteral'][0]
        struct.pack_into('<II', d, off, 4, 0)
        with self.assertRaises(ValueError): w.Metadata39(d).literal(0)

    def test_declaring_type_must_match_type_range(self):
        d = metadata_fixture(); off = struct.unpack_from('<I', d, 8+12*w.SECTIONS.index('methods'))[0]
        struct.pack_into('<H', d, off+4, 1)
        with self.assertRaises(ValueError): w.Metadata39(d)


class ELFTests(unittest.TestCase):
    def test_relocations_map_virtual_addresses_not_file_offsets(self):
        m = w.Metadata39(metadata_fixture()); e = w.Arm64ELF(elf_fixture())
        e.method_map(m)
        self.assertEqual(m.methods[0]['address'], 0x1180)
        self.assertEqual(e.raw(0x2280, 8), 0x680)

    def test_non_executable_method_and_ambiguous_module_fail(self):
        d = elf_fixture(); struct.pack_into('<q', d, 0x700+48+16, 0x2100)
        with self.assertRaises(ValueError): w.Arm64ELF(d).method_map(w.Metadata39(metadata_fixture()))
        d = elf_fixture(); struct.pack_into('<Q', d, 0x700+24, 0x2200)
        with self.assertRaises(ValueError): w.Arm64ELF(d)

    def test_truncated_program_header_and_wrong_arch_fail(self):
        with self.assertRaises(ValueError): w.Arm64ELF(elf_fixture()[:100])
        d = elf_fixture(); struct.pack_into('<H', d, 18, 62)
        with self.assertRaises(ValueError): w.Arm64ELF(d)


class ArtifactTests(unittest.TestCase):
    def test_script_splits_and_object_provenance_are_required(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root/'globalgamemanagers.assets.split0').write_bytes(b'prefix')
            (root/'globalgamemanagers.assets.split1').write_bytes(b'script')
            record = {'script_inventory_sha256': w.digest(b'prefixscript'),
                      'script_object_offset': 6, 'script_object_size': 6,
                      'script_object_sha256': w.digest(b'script')}
            w.verify_settings_script(root, record)
            (root/'globalgamemanagers.assets.split1').rename(root/'globalgamemanagers.assets.split2')
            with self.assertRaises(ValueError): w.verify_settings_script(root, record)

    def test_pins_reject_different_input(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)/'input'; p.write_bytes(b'original')
            record = {'size': 8, 'sha256': hashlib.sha256(b'original').hexdigest()}
            self.assertEqual(w.pinned(p, record), b'original')
            p.write_bytes(b'changed!')
            with self.assertRaises(ValueError): w.pinned(p, record)

    def test_only_pinned_title_field_is_extracted(self):
        # No UnityPy or genuine game files needed in regression tests.
        obj = bytearray(112); struct.pack_into('<iq', obj, 16, 1, 311)
        name = b'PlayFabSharedSettings'; struct.pack_into('<I', obj, 28, len(name)); obj[32:32+len(name)] = name
        at = (32+len(name)+3)&~3
        struct.pack_into('<I', obj, at, 5); obj[at+4:at+9] = b'98AD5'
        data = bytes(208)+obj
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)/'fixture.obb'
            with zipfile.ZipFile(p, 'w') as z: z.writestr('settings', data)
            with zipfile.ZipFile(p) as z: crc = z.getinfo('settings').CRC
            record = {'member': 'settings', 'size': len(data), 'sha256': w.digest(data),
                      'crc32': f'{crc:08x}', 'object_offset': 208, 'object_size': 112}
            self.assertEqual(w.settings_from_obb(p, record), '98AD5')
            record['sha256'] = '0'*64
            with self.assertRaises(ValueError): w.settings_from_obb(p, record)


if __name__ == '__main__': unittest.main()
