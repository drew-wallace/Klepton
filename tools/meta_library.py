"""Account-owned Quest library, authenticated delivery and verified local copies.

Library/current-build/binary-detail queries and header-only CDN transport adapted
from myMartek/avp-play MetaClient.swift (MIT), commit
1a196074151faf40d5f598678e3f26a1cf220c05. See third_party/avp-play/LICENSE.
Only private stdin carries credentials; catalogs/receipts never contain tokens.
"""
from __future__ import annotations
import hashlib
from email.message import Message
from http.client import HTTPException
import json
from pathlib import Path
import re
import shutil
import uuid
import unicodedata
from datetime import datetime, timezone
from urllib.request import Request, build_opener, HTTPRedirectHandler
from urllib.parse import urlencode, urlsplit
from urllib.error import HTTPError, URLError
from steam_library import safe_path as _safe_path


def safe_path(value):
    try:
        return _safe_path(value)
    except ValueError:
        raise ValueError('Unsafe Meta content path.') from None


def identifier(value):
    value = str(value)
    if not re.fullmatch(r'[1-9][0-9]{0,39}', value):
        raise ValueError('Invalid Meta account, application or build ID.')
    return value


def now():
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


class SafeRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urlsplit(newurl)
        # Delivery may redirect to a signed CDN URL. Never forward credentials.
        if (target.scheme != 'https' or target.username or target.password or
            not (target.hostname == 'securecdn.oculus.com' or
                 (target.hostname or '').endswith('.fbcdn.net'))):
            raise ValueError('Meta returned an unsupported download redirect.')
        request = super().redirect_request(req, fp, code, msg, headers, newurl)
        if request:
            request.remove_header('Authorization')
        return request


class Client:
    LIBRARY = '35577972275181723'
    BUILDS = '1586217024733717'
    # The older binary-details document (4734929166632773) returns HTTP 500
    # for current Quest releases. This document lists every asset's ID/size;
    # omitted asset filenames come from the authenticated delivery headers.
    FILES = '24072064135771905'

    def __init__(self, token, check=lambda: None, status=lambda message: None):
        if not isinstance(token, str) or not token or len(token) > 16384 or any(c.isspace() for c in token):
            raise ValueError('Sign in to Meta before refreshing or downloading.')
        self.token, self.check = token, check
        self.status = status
        self._delivery_token = None
        self.opener = build_opener(SafeRedirect())

    def open(self, url, token=None, data=None, headers=None, operation='request'):
        self.check()
        endpoint = urlsplit(url)
        if (endpoint.scheme != 'https' or endpoint.hostname not in ('graph.oculus.com', 'securecdn.oculus.com') or
            endpoint.port not in (None, 443) or endpoint.username or endpoint.password):
            raise ValueError('Invalid Meta service address.')
        request = Request(url, data=data, headers=headers or {})
        if token is not False:
            request.add_header('Authorization', 'Bearer ' + (token or self.token))
        try:
            return self.opener.open(request, timeout=30)
        except HTTPError as error:
            error.close()
            if error.code in (401, 403):
                raise ValueError('Meta rejected access. Sign in again and check that this account has access to the game.') from None
            raise ValueError(f'Meta {operation} failed (HTTP {error.code}). Retry the operation.') from None
        except (URLError, TimeoutError, OSError):
            raise ValueError('Could not reach Meta. Check your connection and retry.') from None

    def graph(self, path, form=None, operation='request'):
        data = urlencode(form).encode() if form is not None else None
        with self.open('https://graph.oculus.com/' + path, data=data,
                       headers={'Content-Type': 'application/x-www-form-urlencoded'}, operation=operation) as response:
            raw = response.read(8*1024*1024+1)
        if len(raw) > 8*1024*1024:
            raise ValueError('Meta returned an oversized response.')
        try:
            result = json.loads(raw)
        except ValueError:
            raise ValueError('Meta returned an invalid response.') from None
        if not isinstance(result, dict):
            raise ValueError('Meta returned an invalid response.')
        if result.get('error') or result.get('errors'):
            code = result.get('error', {}).get('code') if isinstance(result.get('error'), dict) else None
            raise ValueError('Meta session expired. Sign in again.' if code == 190 else 'Meta could not complete this library request.')
        return result

    def query(self, doc, variables):
        operation = {self.LIBRARY: 'library lookup', self.BUILDS: 'build lookup', self.FILES: 'file lookup'}.get(doc, 'request')
        return self.graph('graphql', {'doc_id': doc, 'variables': json.dumps(variables)}, operation=operation)

    def me(self):
        value = self.graph('me?fields=id,alias', operation='account check')
        return identifier(value.get('id', '')), str(value.get('alias') or 'Meta account')[:160]

    @staticmethod
    def library_page(answer):
        try:
            connection = answer['data']['node']['app_entitlements']
            edges, info = connection['edges'], connection['page_info']
            if not isinstance(edges, list) or type(info['has_next_page']) is not bool:
                raise KeyError()
            apps = []
            for edge in edges:
                item = edge['node']['item']
                apps.append({'app_id': identifier(item['id']), 'name': str(item.get('display_name') or item['id'])[:200],
                             'grant': str((item.get('entitlement') or {}).get('grant_reason') or '')[:64]})
            cursor = info.get('end_cursor') if info['has_next_page'] else None
            if info['has_next_page'] and (not isinstance(cursor, str) or not re.fullmatch(r'[A-Za-z0-9_=/+\-]{1,2048}', cursor)):
                raise KeyError()
            return apps, cursor
        except (KeyError, TypeError, ValueError, AttributeError):
            raise ValueError('Meta library response changed. No partial library was saved.') from None

    def library(self, account):
        apps, seen = {}, set()
        cursor = None
        for _ in range(100):
            variables = dict(id=identifier(account), count=50, platform='ANDROID_6DOF',
                             orderBy='display_name_alphabetical', excludeCategories=['ENVIRONMENTS', 'INTERNAL', 'TRACKED_KEYBOARD'])
            if cursor:
                variables['cursor'] = cursor
            page, cursor = self.library_page(self.query(self.LIBRARY, variables))
            apps.update((app['app_id'], app) for app in page)
            if cursor is None:
                return sorted(apps.values(), key=lambda app: app['name'].casefold())
            if cursor in seen:
                break
            seen.add(cursor)
        raise ValueError('Meta library pagination did not complete. The previous library is unchanged.')

    def latest(self, app):
        answer = self.query(self.BUILDS, {'id': identifier(app)})
        try:
            nodes = [edge['node'] for edge in answer['data']['node']['supportedBinaries']['edges']]
            builds = [{'build_id': identifier(b['id']), 'version': str(b.get('version') or b['versionCode'])[:60],
                       'version_code': int(b['versionCode'])} for b in nodes if int(b.get('versionCode') or 0) > 0]
            return max(builds, key=lambda b: b['version_code']) if builds else None
        except (KeyError, TypeError, ValueError, AttributeError):
            return None

    @staticmethod
    def files(answer, build, asset_name=None):
        try:
            binary = answer['data']['node']
            if identifier(binary['id']) != identifier(build):
                raise KeyError()
            items = [(binary, 'apk')]
            if binary.get('obb_binary'):
                items.append((binary['obb_binary'], 'obb'))
            assets = binary.get('asset_files') or {}
            nodes = assets.get('nodes')
            if nodes is None:
                nodes = [edge['node'] for edge in assets.get('edges', [])]
            if (assets.get('count') is not None and int(assets['count']) > len(nodes)) or assets.get('page_info', {}).get('has_next_page'):
                raise ValueError('Meta returned only part of this build’s asset list. Download stopped to avoid missing game data.')
            # Download every listed asset, including files with an IAP association.
            items += [(item, 'asset') for item in nodes]
            files, names = [], set()
            for item, kind in items:
                file_id, size = identifier(item['id']), int(item['size'])
                if size <= 0:
                    raise KeyError()
                name = item.get('file_name')
                if not name and kind == 'asset' and asset_name:
                    name = asset_name(file_id, size)
                name = str(safe_path(name))
                if kind == 'apk' and (not name.lower().endswith('.apk') or '/' in name):
                    raise KeyError()
                relative = name if kind == 'apk' else 'game data/' + name
                normalized = unicodedata.normalize('NFC', relative).casefold()
                if normalized in names:
                    raise ValueError('Meta build contains conflicting file names.')
                names.add(normalized)
                files.append(dict(id=file_id, path=relative, size=size))
            return files[:1] + sorted(files[1:], key=lambda record: record['path'])
        except (KeyError, TypeError, AttributeError):
            raise ValueError('Meta did not return a complete downloadable build.') from None

    def build_files(self, build):
        answer = self.query(self.FILES, {'binaryID': identifier(build)})
        resolved = 0
        def resolve(file_id, size):
            nonlocal resolved
            resolved += 1
            self.status(f'Resolving Meta asset filename {resolved}…')
            return self.asset_name(file_id, size)
        return self.files(answer, build, resolve)

    def asset_name(self, file_id, size):
        url = 'https://securecdn.oculus.com/binaries/download/?' + urlencode({'id': identifier(file_id)})
        # Close after headers, even when the CDN ignores Range. No asset body
        # needs to be downloaded just to discover its original filename.
        with self.open(url, token=self.delivery_token(), headers={'Range': 'bytes=0-0'}, operation='asset lookup') as response:
            if response.status == 206:
                valid = response.headers.get('Content-Range') == f'bytes 0-0/{size}'
            else:
                valid = response.status == 200 and response.headers.get('Content-Length') == str(size)
            if not valid:
                raise ValueError('Meta asset size differs from the selected build.')
            header = Message()
            header['Content-Disposition'] = response.headers.get('Content-Disposition', '')
            name = header.get_filename()
            if not name:
                raise ValueError('Meta did not provide an asset filename. Download stopped to avoid missing game data.')
            return str(safe_path(name))

    def delivery_token(self):
        if self._delivery_token is not None:
            return self._delivery_token
        result = self.graph('authenticate_application?app_id=1481000308606657', {}, operation='download session')
        token = result.get('access_token')
        if not isinstance(token, str) or not token or any(c.isspace() for c in token):
            raise ValueError('Meta did not grant a download session. Sign in again.')
        self._delivery_token = token
        return token

    def download(self, record, destination, token, progress):
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink():
            raise ValueError('Unsafe Meta download path.')
        offset = destination.stat().st_size if destination.exists() else 0
        if offset > record['size']:
            destination.unlink()
            offset = 0
        headers = {'Range': f'bytes={offset}-'} if offset else {}
        url = 'https://securecdn.oculus.com/binaries/download/?' + urlencode({'id': identifier(record['id'])})
        if offset == record['size']:
            return
        with self.open(url, token=token, headers=headers, operation='file download') as response:
            status = response.status
            if status == 206:
                match = re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)', response.headers.get('Content-Range', ''))
                if not match or int(match[1]) != offset or int(match[3]) != record['size'] or int(match[2]) != record['size']-1:
                    raise ValueError('Meta sent an invalid download range. Retry the download.')
            elif status == 200:
                offset = 0
            else:
                raise ValueError('Meta did not deliver the requested file.')
            expected = response.headers.get('Content-Length')
            if expected and int(expected) != record['size'] - offset:
                raise ValueError('Meta download size differs from the selected build.')
            with destination.open('ab' if offset else 'wb') as output:
                while True:
                    self.check()
                    try:
                        chunk = response.read(1024*1024)
                    except (OSError, TimeoutError, HTTPException):
                        raise ValueError('Meta download interrupted. Retry to resume it.') from None
                    if not chunk:
                        break
                    if offset + len(chunk) > record['size']:
                        raise ValueError('Meta delivered more data than expected.')
                    output.write(chunk)
                    offset += len(chunk)
                    progress(offset)
        if offset != record['size']:
            raise ValueError('Meta download interrupted. Retry to resume it.')


class LibraryStore:
    def __init__(self, store, digest, write_json):
        self.root = Path(store)/'meta-library'
        self.digest, self.write_json = digest, write_json

    def account(self, account):
        return self.root/'accounts'/identifier(account)

    def pointer(self, account, app):
        return self.account(account)/'apps'/(identifier(app)+'.json')

    def read(self, account):
        try:
            return json.loads((self.account(account)/'catalog.json').read_text())
        except FileNotFoundError:
            return dict(account_id=identifier(account), games=[], checked_at=None)

    def local(self, account, app, verify=True):
        try:
            receipt = json.loads(self.pointer(account, app).read_text())
            if not re.fullmatch(r'[0-9a-f]{32}', receipt['download_id']):
                return None
            folder = self.account(account)/'downloads'/receipt['download_id']
            valid = receipt['account_id'] == identifier(account) and receipt['app_id'] == identifier(app) and bool(receipt['files'])
            for record in receipt['files']:
                file = folder.joinpath(*safe_path(record['path']).parts)
                if (not file.is_file() or file.is_symlink() or folder.resolve() not in file.resolve().parents or
                    file.stat().st_size != record['size'] or (verify and self.digest(file) != record['sha256'])):
                    valid = False
                    break
            return dict(receipt, folder=str(folder), valid=valid)
        except (ValueError, OSError, KeyError, TypeError):
            return None

    def cached(self, account, remote=False):
        catalog = self.read(account)
        games = {g['app_id']: g for g in catalog['games']}
        # Expired entitlements keep the user's downloaded copy visible, but do
        # not claim current access or offer a fresh server download.
        for pointer in (self.account(account)/'apps').glob('*.json'):
            if pointer.stem not in games:
                local = self.local(account, pointer.stem, verify=False)
                if local:
                    games[pointer.stem] = dict(app_id=pointer.stem, name=local['name'], owned=False)
        for game in games.values():
            local = self.local(account, game['app_id'], verify=False)
            game['downloaded'] = ({k: local.get(k) for k in ('build_id', 'version', 'version_code', 'downloaded_at', 'valid')}
                                  if local else None)
            game['remote_known'] = (remote if isinstance(remote, bool) else game['app_id'] in remote) and game.get('latest') is not None and game.get('owned', True)
        catalog['games'] = sorted(games.values(), key=lambda g: g['name'].casefold())
        catalog['remote_known'] = remote is True
        return catalog

    def refresh(self, client, progress, expected_account=None):
        account, name = client.me()
        if expected_account is not None and account != identifier(expected_account):
            raise ValueError('Meta account changed. Sign in again before refreshing.')
        games = client.library(account)
        for index, game in enumerate(games):
            client.check()
            progress(index, len(games))
            try:
                game['latest'] = client.latest(game['app_id'])
            except ValueError as error:
                if 'expired' in str(error) or 'rejected' in str(error):
                    raise
                game['latest'] = None
            game['owned'] = True
        self.write_json(self.account(account)/'catalog.json', dict(account_id=account, account_name=name, games=games, checked_at=now()))
        return self.cached(account, remote=True)

    def acquire(self, client, account, app, inspect, progress):
        actual, _ = client.me()
        if actual != identifier(account):
            raise ValueError('Meta account changed. Refresh the library before downloading.')
        owned = client.library(account)
        game = next((g for g in owned if g['app_id'] == identifier(app)), None)
        if not game:
            raise ValueError('This Meta account no longer has access to this game.')
        latest = client.latest(app)
        if not latest:
            raise ValueError('Meta did not provide an accessible Quest build for this game.')
        files = client.build_files(latest['build_id'])
        identity = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
        pending = self.account(account)/'pending'/(identifier(app)+'-'+identity)
        if pending.is_symlink():
            raise ValueError('Unsafe Meta download folder.')
        pending.mkdir(parents=True, exist_ok=True)
        needed = sum(max(0, r['size'] - ((pending/r['path']).stat().st_size if (pending/r['path']).is_file() else 0)) for r in files)
        if shutil.disk_usage(pending).free < needed + files[0]['size'] + 512*1024**2:
            raise ValueError('Not enough free disk space for this Meta download and APK import.')
        token = client.delivery_token()
        total, done = sum(r['size'] for r in files), 0
        records = []
        for record in files:
            client.check()
            path = pending.joinpath(*safe_path(record['path']).parts)
            if pending.resolve() not in path.resolve().parents:
                raise ValueError('Unsafe Meta content path.')
            client.download(record, path, token, lambda size: progress(done+size, total))
            records.append(dict(path=record['path'], size=record['size'], sha256=self.digest(path)))
            done += record['size']
        info = inspect(pending/files[0]['path'])
        if str(info['version_code']) != str(latest['version_code']):
            raise ValueError('Downloaded APK version does not match Meta’s build. The previous copy is unchanged.')
        manifest_app = info.get('meta_app_id')
        if manifest_app and manifest_app != identifier(app):
            raise ValueError('Downloaded APK belongs to a different Meta application.')
        client.check()
        download_id = uuid.uuid4().hex
        folder = self.account(account)/'downloads'/download_id
        folder.parent.mkdir(parents=True, exist_ok=True)
        receipt = dict(schema_version=1, download_id=download_id, account_id=identifier(account), app_id=identifier(app),
                       name=game['name'], **latest, downloaded_at=now(), files=records, executable=files[0]['path'], import_id=info['import_id'])
        self.write_json(self.account(account)/'receipts'/(download_id+'.json'), receipt)
        pending.replace(folder)
        client.check()
        game.update(latest=latest, owned=True)
        catalog = self.read(account)
        catalog['games'] = [g for g in catalog['games'] if g['app_id'] != app] + [game]
        self.write_json(self.account(account)/'catalog.json', catalog)
        client.check()
        self.write_json(self.pointer(account, app), receipt)
        return info, dict(receipt, folder=str(folder), valid=True), self.cached(account, remote={identifier(app)})
