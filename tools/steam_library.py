"""Installer-owned Steam cache and private helper pipe transport.

Tokens are forwarded only over pipes. This module never logs helper output or
writes authentication requests to disk; receipts contain acquisition data only.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import shutil
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone


def safe_path(value):
    if not isinstance(value, str) or not value or re.search(r'[\x00-\x1f]', value):
        raise ValueError('Unsafe Steam content path.')
    value = value.replace('\\', '/')
    if value.startswith('/') or re.match(r'^[a-zA-Z]:', value) or any(p in ('', '..') for p in value.split('/')):
        raise ValueError('Unsafe Steam content path.')
    path = PurePosixPath(value)
    if str(path) == '.':
        raise ValueError('Unsafe Steam content path.')
    return path


def account_id(value):
    value = str(value)
    if not re.fullmatch(r'\d{17}', value):
        raise ValueError('Sign in to Steam before using the library.')
    return value


def app_id(value):
    value = str(value)
    if not re.fullmatch(r'[1-9]\d{0,9}', value):
        raise ValueError('Invalid Steam application.')
    return value


def node_path(root):
    candidates = [os.environ.get('KLEPTON_NODE'), shutil.which('node'),
                  '/opt/homebrew/bin/node', '/usr/local/bin/node']
    candidates += [str(p) for p in sorted((Path.home()/'.nvm/versions/node').glob('*/bin/node'), reverse=True)]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            result = subprocess.run([candidate, '-p', 'process.versions.node.split(".")[0]'], capture_output=True, timeout=5)
            if result.returncode == 0 and result.stdout.strip().isdigit() and int(result.stdout) >= 22:
                return candidate
    raise ValueError('Steam Library needs Node.js 22 or later. Run brew install node, then npm ci --ignore-scripts --prefix steam/tools/library in the checkout.')


ERRORS = {
    'unsafe_path': 'Steam content contains an unsafe path or symbolic link.',
    'no_android': 'This branch has no accessible Android release. Refresh the library or choose another branch.',
    'no_license': 'The signed-in Steam account does not own this game.',
    'file_collision': 'Steam depots contain conflicting file paths; this release cannot be imported automatically.',
    'missing_apk': 'The selected Android launch APK was not found in the available depots.',
    'checksum': 'Steam download verification failed. Retry the download; the previous copy is unchanged.',
    'expired': 'Steam sign-in timed out. Start sign-in again.',
    'disk_space': 'There is not enough free disk space for this Steam download.',
    'steam_failed': 'Steam could not complete the request. Check your connection and session, then retry or sign in again.',
}


def run_helper(root, request, runner, emit):
    """Relay allowlisted events and interactive Guard input without Runner logging."""
    node = node_path(root)
    helper = root/'steam/tools/library/helper.cjs'
    if not (helper.parent/'node_modules/steam-user/package.json').is_file() or not (helper.parent/'node_modules/steam-session/package.json').is_file():
        raise ValueError('Steam Library dependencies are missing. Run npm ci --ignore-scripts --prefix steam/tools/library in the checkout.')
    process = subprocess.Popen([node, str(helper)], cwd=root, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, start_new_session=True)
    runner.child = process
    selector = selectors.DefaultSelector()
    result = None
    pending = b''
    deadline = time.monotonic() + 240
    try:
        process.stdin.write((json.dumps(request)+'\n').encode())
        process.stdin.flush()
        selector.register(process.stdout, selectors.EVENT_READ, 'helper')
        selector.register(sys.stdin, selectors.EVENT_READ, 'input')
        while process.poll() is None or any(k.data == 'helper' for k in selector.get_map().values()):
            runner.check_cancelled()
            if time.monotonic() > deadline:
                raise ValueError('Steam stopped responding. Retry the operation; completed downloads are unchanged.')
            for key, _ in selector.select(.2):
                if key.data == 'input':
                    line = sys.stdin.readline()
                    if not line:
                        selector.unregister(sys.stdin)
                        continue
                    try:
                        command = json.loads(line)
                        if isinstance(command.get('guard_code'), str):
                            process.stdin.write((json.dumps({'guard_code': command['guard_code']})+'\n').encode())
                            process.stdin.flush()
                    except (ValueError, BrokenPipeError):
                        pass
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    selector.unregister(process.stdout)
                    continue
                pending += chunk
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    try:
                        event = json.loads(line)
                    except ValueError:
                        raise ValueError('Steam helper returned an invalid response.') from None
                    deadline = time.monotonic() + 240
                    kind = event.get('event')
                    if kind == 'credential':
                        emit('steam-auth', 'credential', refresh_token=event['refresh_token'],
                             steam_id=account_id(event['steam_id']), account_name=event.get('account_name', 'Steam'))
                    elif kind == 'auth':
                        challenge = event.get('challenge_url', '')
                        if challenge and not re.fullmatch(r'https://s\.team/q/[^\s]+', challenge):
                            raise ValueError('Steam returned an invalid QR challenge.')
                        emit('steam-auth', 'auth', kind=event['kind'], challenge_url=challenge)
                    elif kind == 'progress':
                        completed, total = int(event.get('completed', 0)), int(event.get('total', 0))
                        text = ('Reading Steam library' if event.get('kind') == 'library' else 'Downloading Steam Android files')
                        detail = ''
                        if event.get('kind') == 'download' and 'network_bps' in event:
                            rate = max(0, int(event['network_bps']))
                            phase = 'Unpacking' if event.get('phase') == 'unpacking' else 'Downloading'
                            percent = min(100, max(0, completed * 100 / total)) if total else 0
                            detail = f' · {percent:.1f}% · Network {rate / 1024**2:.2f} MiB/s · {phase}'
                            eta = event.get('eta_seconds')
                            if isinstance(eta, (int, float)) and eta > 0:
                                detail += f' · About {int(eta) // 60}m {int(eta) % 60}s left'
                            switches = max(0, int(event.get('switches', 0)))
                            stalled = max(0, int(event.get('stalled_seconds', 0)))
                            detail += f' · {max(0, int(event.get("active_hosts", 0)))} CDNs'
                            if switches: detail += f' · {switches} server switches'
                            if stalled >= 5: detail += f' · Last verified progress {stalled}s ago'
                        emit('steam-library', 'running', text + f' ({completed:,} / {total:,})' + detail, completed=completed, total=total)
                    elif kind == 'selection':
                        # Deliberately not published as a completed download.
                        account_id(event['steam_id'])
                    elif kind == 'error':
                        reason = event.get('reason', '')
                        safe_reasons = {'CDN request timed out', 'File checksum mismatch', 'Chunk checksum mismatch',
                                        'Chunk decompression failed', 'CDN transfer failed', 'ENOSPC', 'EIO', 'EACCES',
                                        'ECONNRESET', 'ETIMEDOUT', 'ENOTFOUND', 'Slow CDN connection replaced', 'No verified download progress'}
                        detail = ' (' + reason + ')' if reason in safe_reasons or re.fullmatch(r'HTTP [0-9]{3}', reason) else ''
                        raise ValueError(ERRORS.get(event.get('code'), ERRORS['steam_failed']) + detail)
                    elif kind == 'result':
                        result = {k: event[k] for k in ('steam_id', 'games', 'game', 'branch', 'launch', 'files',
                                                       'owned_app_ids', 'checked_app_ids') if k in event}
        runner.check_cancelled()
        if process.wait() or result is None:
            raise ValueError(ERRORS['steam_failed'])
        account_id(result['steam_id'])
        return result
    finally:
        selector.close()
        runner.stop_child()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, 9)
            process.wait()
        runner.child = None
        process.stdin.close()
        process.stdout.close()


class LibraryStore:
    def __init__(self, store, digest, write_json):
        self.root = Path(store)/'steam-library'
        self.digest = digest
        self.write_json = write_json

    def account(self, steam_id):
        return self.root/'accounts'/account_id(steam_id)

    def pointer(self, steam_id, app, branch, launch):
        key = hashlib.sha256(json.dumps([str(branch), str(launch)]).encode()).hexdigest()
        return self.account(steam_id)/'apps'/app_id(app)/(key+'.json')

    def local(self, steam_id, app, branch, launch, verify=True):
        pointer = self.pointer(steam_id, app, branch, launch)
        try:
            receipt = json.loads(pointer.read_text())
            identifier = receipt['download_id']
            if not re.fullmatch(r'[0-9a-f]{32}', identifier):
                return None
            folder = self.account(steam_id)/'downloads'/identifier
            valid = bool(receipt.get('files')) and receipt.get('steam_id') == str(steam_id) and receipt.get('app_id') == str(app)
            for record in receipt['files']:
                file = folder.joinpath(*safe_path(record['path']).parts)
                if not file.is_file() or file.is_symlink() or folder.resolve() not in file.resolve().parents or file.stat().st_size != record['size']:
                    valid = False
                    break
                if verify and self.digest(file) != record['sha256']:
                    valid = False
                    break
            return dict(receipt, valid=valid, folder=str(folder))
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def decorate(self, steam_id, games, remote_known):
        for game in games:
            for branch in game['branches']:
                branch['downloaded'] = {}
                branch['remote_known'] = remote_known if isinstance(remote_known, bool) else game['app_id'] in remote_known
                for launch in game['launches']:
                    local = self.local(steam_id, game['app_id'], branch['name'], launch['id'])
                    if local:
                        branch['downloaded'][launch['id']] = {k: local.get(k) for k in
                            ('build_id', 'downloaded_at', 'revision', 'valid', 'import_id')}
        return games

    def read_catalog(self, steam_id):
        try:
            return json.loads((self.account(steam_id)/'catalog.json').read_text())
        except FileNotFoundError:
            return {'steam_id': account_id(steam_id), 'games': []}

    def cached(self, steam_id, remote_known=False):
        catalog = self.read_catalog(steam_id)
        catalog['games'] = self.decorate(steam_id, catalog['games'], remote_known)
        return catalog

    def save_catalog(self, result):
        catalog = {'steam_id': account_id(result['steam_id']), 'games': result['games']}
        checked = result.get('checked_app_ids')
        if checked is not None:
            owned = set(result['owned_app_ids'])
            catalog['games'] += [g for g in self.read_catalog(result['steam_id'])['games']
                                 if g['app_id'] in owned and g['app_id'] not in checked]
            catalog['games'].sort(key=lambda g: g['name'].casefold())
        if 'owned_app_ids' in result:
            catalog['owned_app_ids'] = result['owned_app_ids']
        self.write_json(self.account(result['steam_id'])/'catalog.json', catalog)
        return self.cached(result['steam_id'], set(checked) if checked is not None else True)

    def publish(self, result, pending, inspect, check_cancelled=lambda: None):
        steam_id = account_id(result['steam_id'])
        game, branch, launch = result['game'], result['branch'], result['launch']
        # Recheck filesystem boundaries and all payload hashes before publication.
        for record in result['files']:
            check_cancelled()
            file = pending.joinpath(*safe_path(record['path']).parts)
            if file.is_symlink() or pending.resolve() not in file.resolve().parents or not file.is_file() or file.stat().st_size != record['size'] or self.digest(file) != record['sha256']:
                raise ValueError('Downloaded Steam files failed verification.')
        apk = pending.joinpath(*safe_path(launch['executable']).parts)
        info = inspect(apk)
        check_cancelled()
        identifier = uuid.uuid4().hex
        folder = self.account(steam_id)/'downloads'/identifier
        folder.parent.mkdir(parents=True, exist_ok=True)
        receipt = dict(schema_version=1, download_id=identifier, steam_id=steam_id, app_id=app_id(game['app_id']),
                       branch=branch['name'], launch_id=launch['id'], executable=launch['executable'],
                       build_id=branch['build_id'], revision=branch['revision'], depots=branch['depots'],
                       remote_updated_at=branch['updated_at'], downloaded_at=datetime.now(timezone.utc).isoformat(timespec='seconds'),
                       files=result['files'], import_id=info['import_id'])
        # Update just the freshly resolved game, including branch changes during the download.
        cached = self.cached(steam_id)
        cached['games'] = [g for g in cached['games'] if g['app_id'] != game['app_id']] + [game]
        self.write_json(self.account(steam_id)/'catalog.json', cached)
        check_cancelled()
        pending.replace(folder)
        check_cancelled()
        self.write_json(self.pointer(steam_id, game['app_id'], branch['name'], launch['id']), receipt)
        catalog = self.cached(steam_id, {game['app_id']})
        return info, receipt, folder, catalog


def data_candidates(folder, receipt):
    """Only automatically attach Valve's documented adjacent obb directory."""
    obb = folder/'obb'
    attached = [str(obb)] if obb.is_dir() and any(obb.iterdir()) else []
    unmapped = [f['path'] for f in receipt['files']
                if not f['path'].startswith('obb/') and f['path'] != receipt['executable'] and not f['path'].lower().endswith('.apk')]
    return attached, unmapped
