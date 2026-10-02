#!/usr/bin/env python3
"""Inspect a pinned Steam Frame recovery archive without mounting or booting it.

Only the rootfs-A partition is retained. Valve images stay in ignored build/.
ZIP CRCs and GPT CRCs detect corruption; the observed SHA pin is not a separately
verified Valve signature. Btrfs parsing uses the pinned optional Python modules.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import tarfile
import zipfile
import zlib

REPO = Path(__file__).resolve().parents[2]
ROOT = REPO / 'build/steam-runtime/steamframe-0.3.0'
LOCK = REPO / 'steam/tools/steamframe_recovery.lock.json'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while data := f.read(8 * 1024 * 1024):
            h.update(data)
    return h.hexdigest()


def partitions(head, image_size):
    for sector in (512, 4096):
        if head[sector:sector+8] != b'EFI PART':
            continue
        size, crc = struct.unpack_from('<II', head, sector+12)
        if size < 92 or size > sector:
            raise ValueError('invalid GPT header size')
        header = bytearray(head[sector:sector+size]); header[16:20] = bytes(4)
        if zlib.crc32(header) != crc:
            raise ValueError('GPT header CRC mismatch')
        lba = struct.unpack_from('<Q', head, sector+72)[0]
        count, entry_size, entries_crc = struct.unpack_from('<III', head, sector+80)
        if entry_size < 128 or count * entry_size > len(head):
            raise ValueError('invalid GPT entry table')
        entries = head[lba*sector:lba*sector+count*entry_size]
        if len(entries) != count*entry_size or zlib.crc32(entries) != entries_crc:
            raise ValueError('GPT entries CRC mismatch')
        result = []
        for i in range(count):
            entry = entries[i*entry_size:(i+1)*entry_size]
            if not any(entry[:16]):
                continue
            first, last = struct.unpack_from('<QQ', entry, 32)
            start, size = first*sector, (last-first+1)*sector
            if last < first or start+size > image_size:
                raise ValueError('partition outside image')
            result.append({'index':i+1, 'name':entry[56:128].decode('utf-16le').rstrip('\0'),
                           'offset':start, 'size':size, 'sector_bytes':sector})
        return result
    raise ValueError('no GPT found')


def prepare(archive, out, lock):
    if archive.stat().st_size != lock['size'] or digest(archive) != lock['sha256']:
        raise ValueError('archive does not match pinned size/SHA-256')
    out.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        if len(z.infolist()) != 1 or z.infolist()[0].filename != 'lun0.img':
            raise ValueError('unexpected recovery ZIP members')
        info = z.infolist()[0]
        if info.file_size != lock['image_size']:
            raise ValueError('unexpected disk image size')
        with z.open(info) as src:
            head = src.read(1024*1024)
            table = partitions(head, info.file_size)
            rootfs = next(p for p in table if p['name'] == 'rootfs-A')
            start, end = rootfs['offset'], rootfs['offset']+rootfs['size']
            image_hash, root_hash = hashlib.sha256(), hashlib.sha256()
            target = out/'rootfs-A.img.part'
            position = 0
            with target.open('wb') as dst:
                data = head
                while data:
                    image_hash.update(data)
                    lo, hi = max(start, position), min(end, position+len(data))
                    if hi > lo:
                        chunk = data[lo-position:hi-position]
                        dst.write(chunk); root_hash.update(chunk)
                    position += len(data)
                    data = src.read(8*1024*1024)
            if position != info.file_size or target.stat().st_size != rootfs['size']:
                raise ValueError('truncated recovery image')
            # Reading the complete ZIP member also verifies its CRC.
            target.replace(out/'rootfs-A.img')
    receipt = {'source':lock, 'image_sha256':image_hash.hexdigest(), 'zip_crc32':f'{info.CRC:08x}',
               'partitions':table, 'rootfs_sha256':root_hash.hexdigest(),
               'execution':'not_run', 'login':'not_run', 'tickets':'not_run'}
    (out/'image-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Verified archive, ZIP CRC, GPT CRCs; extracted rootfs-A:',out/'rootfs-A.img')


def open_filesystem(out):
    from dissect.btrfs import Btrfs
    receipt = json.loads((out/'image-receipt.json').read_text())
    image = out/'rootfs-A.img'
    if digest(image) != receipt['rootfs_sha256']:
        raise ValueError('root filesystem hash mismatch')
    return image.open('rb'), Btrfs


def posix_member(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts:
        raise ValueError('invalid filesystem member')
    return path


def catalog(out):
    fh, Btrfs = open_filesystem(out)
    records = []
    def walk(node, parent):
        for name, child in node.iterdir():
            if name in ('.', '..'): continue
            relative = posix_member(name)
            if len(relative.parts) != 1: raise ValueError('invalid directory entry')
            path = parent+'/'+name
            kind = 'directory' if child.is_dir() else 'file' if child.is_file() else 'symlink' if child.is_symlink() else 'other'
            record = {'path':path,'type':kind,'size':child.size}
            if child.is_symlink(): record['target'] = child.link
            records.append(record)
            if child.is_dir(): walk(child,path)
    with fh:
        fs = Btrfs(fh); walk(fs.root,'')
    (out/'filesystem.json').write_text(json.dumps(records,indent=2)+'\n')
    (out/'rootfs-files.txt').write_text('\n'.join(f"{r['type']:10} {r['size']:12} {r['path']}" for r in records)+'\n')
    print('Catalogued',len(records),'filesystem entries')


def write_member(src, destination, expected_size, root):
    if destination.is_symlink() or not destination.resolve().is_relative_to(root.resolve()):
        raise ValueError('unsafe extraction destination')
    destination.parent.mkdir(parents=True,exist_ok=True)
    h = hashlib.sha256(); count = 0
    temporary = destination.with_name(destination.name+'.part')
    if temporary.is_symlink(): raise ValueError('unsafe temporary destination')
    with temporary.open('wb') as dst:
        while data := src.read(min(8*1024*1024,expected_size-count+1)):
            count += len(data)
            if count > expected_size: raise ValueError('oversize member')
            dst.write(data); h.update(data)
    if count != expected_size: raise ValueError('truncated member')
    temporary.replace(destination)
    return {'size':count,'sha256':h.hexdigest()}


def extract(out):
    from backports.zstd import ZstdFile
    from steam_runtime import archive_path, elf_inventory
    records = json.loads((out/'filesystem.json').read_text())
    destination = out/'extracted'; manifest = []
    fh, Btrfs = open_filesystem(out)
    with fh:
        fs = Btrfs(fh)
        for record in records:
            path = record['path']
            selected = (path in ['/usr/lib/steam/steam.tar.zst','/usr/lib/steam/bin_steam.sh','/etc/os-release']
                        or record['size'] < 2000000 and any(s in path for s in
                        ['/usr/lib/steamos/','/usr/bin/steam','/etc/steamos','/etc/xdg/autostart/steam',
                         '/usr/lib/systemd/user/steam','/usr/lib/systemd/user/podman.service.d/lepton']))
            if record['type'] != 'file' or not selected: continue
            relative = posix_member(path.lstrip('/'))
            with fs.get(path).open() as src:
                item = write_member(src,destination/relative,record['size'],destination)
            manifest.append({'source_path':path,**item})
    (out/'extracted-rootfs-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    selected = []; inventory = []; entries = []
    destination = out/'steam'
    with ZstdFile(out/'extracted/usr/lib/steam/steam.tar.zst','rb') as stream, tarfile.open(fileobj=stream,mode='r|') as tar:
        for member in tar:
            relative = archive_path(member.name); name = str(relative)
            entries.append({'path':name,'size':member.size,'type':member.type.decode('ascii',errors='replace')})
            if not (name.startswith(('androidarm64/','linuxarm64/','steamrtarm64/'))
                    or name.startswith(('steamui/','clientui/')) and name.endswith(('.js','.html','.css','.json','.map'))
                    or name in
                    ['steam.sh','RUNSTEAM.sh','steam_msg.sh','package/beta']): continue
            if member.isdir(): continue
            if member.issym() or member.islnk():
                selected.append({'path':name,'type':'symlink' if member.issym() else 'hardlink','target':member.linkname})
                continue  # Links remain metadata; never create host links from the image.
            if not member.isfile() or member.size > 1024**3: raise ValueError('invalid selected tar member')
            with tar.extractfile(member) as src:
                item = write_member(src,destination/relative,member.size,destination)
            selected.append({'path':name,'type':'file',**item})
            target = destination/relative
            with target.open('rb') as f: magic = f.read(4)
            if magic == b'\x7fELF': inventory.append({'path':name,**item,**elf_inventory(target.read_bytes())})
    (out/'steam-package-index.json').write_text(json.dumps(entries,indent=2)+'\n')
    (out/'steam-extraction.json').write_text(json.dumps(selected,indent=2)+'\n')
    (out/'steam-elf-inventory.json').write_text(json.dumps(inventory,indent=2)+'\n')
    print('Extracted',len(manifest),'rootfs files and',len(selected),'Steam entries;',len(inventory),'ELF files')


def dependencies(out):
    """Retain the Linux ELF dependency closure for analysis, not execution."""
    from steam_runtime import elf_inventory
    inventory = json.loads((out/'steam-elf-inventory.json').read_text())
    package = {r['path']:r for r in inventory}
    links = {r['path']:r['target'] for r in json.loads((out/'steam-extraction.json').read_text()) if r['type']=='symlink'}
    import posixpath
    def bundled(name):
        for folder in ('steamrtarm64','steamrtarm64/libs','linuxarm64'):
            path = folder+'/'+name
            for _ in range(40):
                if path not in links: break
                path = posixpath.normpath(posixpath.join(posixpath.dirname(path),links[path]))
            if path in package: return package[path]
        return None
    queue = [r for r in inventory if r['path'] in
             ('steamrtarm64/steam','steamrtarm64/steamclient.so','steamrtarm64/steamui.so',
              'steamrtarm64/libSDL3.so.0','steamrtarm64/crashhandler.so','steamrtarm64/steamwebhelper','steamrtarm64/libcef.so')]
    visited = set(); system = {}; edges = []; missing = set()
    destination = out/'system-dependencies'; fh, Btrfs = open_filesystem(out)
    with fh:
        fs = Btrfs(fh)
        while queue:
            current = queue.pop(0)
            if current['path'] in visited: continue
            visited.add(current['path'])
            names = list(current['needed'])
            if current['interpreter']: names.append(posixpath.basename(current['interpreter']))
            for name in names:
                if '/' in name or name in ('.','..'): raise ValueError('unsafe dependency soname')
                dependency = bundled(name) or system.get(name)
                if dependency is None:
                    for folder in ('/usr/lib','/usr/lib/pulseaudio'):  # Frame /lib aliases /usr/lib.
                        path = folder+'/'+name
                        try: node = fs.get(path)
                        except FileNotFoundError: continue
                        for _ in range(40):
                            if not node.is_symlink(): break
                            path = posixpath.normpath(posixpath.join(posixpath.dirname(path),node.link))
                            node = fs.get(path)
                        if node.is_symlink(): raise ValueError('dependency symlink loop')
                        if not node.is_file(): continue
                        with node.open() as src:
                            item = write_member(src,destination/name,node.size,destination)
                        metadata = elf_inventory((destination/name).read_bytes())
                        if metadata is None: raise ValueError('non-ELF dependency')
                        dependency = {'path':path,'canonical_path':path,**item,**metadata}
                        system[name] = dependency; break
                edges.append({'from':current['path'],'needed':name,'resolved':dependency['path'] if dependency else None})
                if dependency: queue.append(dependency)
                else: missing.add(name)
    result = {'roots':[r['path'] for r in inventory if r['path'] in visited],
              'system_libraries':list(system.values()),'edges':edges,'missing':sorted(missing),
              'scope':'Static DT_NEEDED/PT_INTERP Linux closure; excludes runtime dlopen and process services',
              'execution':'not_run'}
    (out/'dependency-closure.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Retained',len(system),'Linux system libraries; unresolved DT_NEEDED:',sorted(missing))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive',type=Path)
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--lock',type=Path,default=LOCK)
    parser.add_argument('--stage',choices=['prepare','catalog','extract','dependencies','all'],default='all')
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text())
    if args.stage in ('prepare','all'): prepare(args.archive or args.root/lock['filename'],args.root,lock)
    if args.stage in ('catalog','all'): catalog(args.root)
    if args.stage in ('extract','all'): extract(args.root)
    if args.stage in ('dependencies','all'): dependencies(args.root)

if __name__ == '__main__':
    main()
