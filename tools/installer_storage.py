"""Inventory and cleanup for installer-owned files only. Caller holds job.lock."""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat


def safe(root, path):
    """Never read through or delete through a symbolic-link ancestor."""
    try:
        relative = path.relative_to(root)
    except ValueError:
        return False
    current = root
    if current.is_symlink():
        return False
    for part in relative.parts:
        current = current/part
        if current.is_symlink():
            return False
    return True


def read(root, path):
    if not safe(root, path):
        raise ValueError('Storage metadata contains a symbolic link.')
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError('Invalid storage metadata.')
    return value


def measure(path):
    """Logical file bytes; do not follow links or count linked originals."""
    total = 0
    revision = hashlib.sha256()
    if path.is_file() and not path.is_symlink():
        info = path.stat()
        return info.st_size, f'{info.st_ino}:{info.st_mtime_ns}:{info.st_size}'
    def unreadable(error):
        raise error

    for directory, directories, files in os.walk(path, followlinks=False, onerror=unreadable):
        directories.sort()
        for name in sorted(directories + files):
            info = (Path(directory)/name).lstat()
            revision.update(f'{Path(directory).relative_to(path)/name}:{info.st_ino}:{info.st_mtime_ns}:{info.st_size}'.encode())
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
    return total, revision.hexdigest()


def size(path):
    return measure(path)[0]


def inventory(store):
    root = Path(store).absolute()
    if root.is_symlink():
        raise ValueError('Installer storage must not be a symbolic link.')
    entries = []
    warnings = []
    # Recovery may need both the original import and downloaded game data.
    recovery = any((root/'pending').glob('*'))
    if recovery:
        warnings.append('An interrupted install has pending recovery. Downloads, imports and build files are protected until saved data is restored.')

    def add(path, category, name, detail, date=None, protected=False, pointers=()):
        if not path.exists() or not safe(root, path):
            return
        info = path.stat()
        relative = path.relative_to(root).as_posix()
        bytes_used, revision = measure(path)
        identifier = hashlib.sha256(f'{relative}:{info.st_ino}:{info.st_mtime_ns}:{revision}'.encode()).hexdigest()
        entries.append(dict(id=identifier, path=str(path), category=category, name=name,
                            detail=detail, bytes=bytes_used, protected=protected,
                            date=date or datetime.fromtimestamp(info.st_mtime, timezone.utc).isoformat(),
                            pointers=[str(p) for p in pointers]))

    for service in ('steam', 'meta'):
        library = root/(service+'-library')
        if not safe(root, library):
            continue
        for account in sorted((library/'accounts').glob('*')):
            if not account.is_dir() or not safe(root, account):
                continue
            receipts = {}
            names = {}
            try:
                names = {str(g['app_id']): g['name'] for g in read(root, account/'catalog.json').get('games', [])}
            except (OSError, ValueError, KeyError, TypeError):
                pass
            uncertain = False
            for pointer in sorted((account/'apps').rglob('*.json')):
                try:
                    receipt = read(root, pointer)
                    identifier = receipt['download_id']
                    if not isinstance(identifier, str) or not re.fullmatch(r'[0-9a-f]{32}', identifier):
                        raise ValueError('Invalid download identifier.')
                    receipts.setdefault(identifier, []).append((pointer, receipt))
                except (OSError, ValueError, KeyError, TypeError):
                    uncertain = True
            if uncertain:
                warnings.append(f'{service.title()} account {account.name} has unreadable receipts; its downloads are protected.')
            for folder in sorted((account/'downloads').glob('*')):
                pointers = receipts.get(folder.name, [])
                receipt = pointers[0][1] if pointers else {}
                if not receipt:
                    try:
                        receipt = read(root, account/'receipts'/(folder.name+'.json'))
                    except (OSError, ValueError):
                        pass
                name = receipt.get('name') or names.get(str(receipt.get('app_id'))) or 'Unreferenced download'
                version = receipt.get('version') or receipt.get('version_name') or receipt.get('version_code')
                detail = ' · '.join(str(v) for v in [
                    'Current download' if pointers else 'Older or unused download',
                    receipt.get('branch'), version,
                    f'Build {receipt["build_id"]}' if receipt.get('build_id') else None,
                    f'Account {account.name}'] if v)
                add(folder, service.title()+' downloads', name, detail,
                    receipt.get('downloaded_at'), recovery or uncertain, [p for p, _ in pointers])
            for folder in sorted((account/'pending').glob('*')):
                add(folder, 'Interrupted downloads', service.title()+' partial download',
                    f'Retry to resume, or delete to start again · Account {account.name}', protected=recovery)
        for folder in sorted(library.glob('pending-*')):
            add(folder, 'Interrupted downloads', service.title()+' partial download',
                'Incomplete transfer', protected=recovery)
    for folder in sorted((root/'imports').glob('*')):
        metadata = {}
        try:
            metadata = read(root, folder/'inspection.json')
        except (OSError, ValueError):
            pass
        version = metadata.get('version_name') or metadata.get('version_code')
        add(folder, 'Imported APKs', metadata.get('name') or 'Incomplete APK import',
            ' · '.join(str(v) for v in [metadata.get('package'), version,
                                       'Local APK copy and unpacked files'] if v), protected=recovery)
    for job in sorted((root/'jobs').glob('*')):
        if not safe(root, job):
            continue
        for name in ('DerivedData', 'payload', 'post-install-saved-data-readback'):
            add(job/name, 'Build files', f'Install {job.name}', name, protected=recovery)
    total = size(root) if root.exists() else 0
    free = shutil.disk_usage(root if root.exists() else root.parent).free
    return dict(root=str(root), entries=entries, total_bytes=total, free_bytes=free,
                removable_bytes=sum(e['bytes'] for e in entries if not e['protected']), warnings=warnings)


def cleanup(store, identifiers):
    if not isinstance(identifiers, list) or not identifiers or not all(isinstance(i, str) for i in identifiers):
        raise ValueError('Select storage items to delete.')
    snapshot = inventory(store)
    by_id = {e['id']: e for e in snapshot['entries']}
    selected = []
    for identifier in dict.fromkeys(identifiers):
        entry = by_id.get(identifier)
        if entry is None or entry['protected']:
            raise ValueError('Storage changed or an item is protected. Refresh and select the items again.')
        selected.append(entry)
    # Validate the whole request before deleting any item. Metadata and recovery
    # evidence are retained; remove only pointers to the selected downloads.
    for entry in selected:
        path = Path(entry['path'])
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink()
        for pointer in entry['pointers']:
            Path(pointer).unlink()
    return dict(inventory(store), removed_bytes=sum(e['bytes'] for e in selected), removed_count=len(selected))
