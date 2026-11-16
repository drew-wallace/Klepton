"""Meta library completeness, private delivery, cache integrity and build identity."""
import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from urllib.request import Request
from urllib.error import HTTPError
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import utility as u
import meta_library as m
import meta_game

ACCOUNT, OTHER, APP = '123456789', '987654321', '2370815932930055'
BUILD = dict(build_id='111', version='1.2', version_code=12)


def library_page(items, more=False, cursor=None):
    return {'data': {'node': {'app_entitlements': {'edges': [
        {'node': {'item': dict(id=app, display_name=name, entitlement={'grant_reason': 'PURCHASED'})}}
        for app, name in items], 'page_info': {'has_next_page': more, 'end_cursor': cursor}}}}}


def binary(assets=None):
    return {'data': {'node': {'id': '111', 'file_name': 'game.apk', 'size': '3',
            'obb_binary': {'id': '112', 'file_name': 'main.12.test.game.obb', 'size': 4},
            'asset_files': assets or {'count': 1, 'nodes': [{'id': '113', 'file_name': 'required.bundle', 'size': 6, 'iap_item': {'id': '555'}}]}}}}


class Response(io.BytesIO):
    def __init__(self, data, status=200, headers=None):
        super().__init__(data)
        self.status, self.headers = status, headers or {}


class FakeClient:
    check = staticmethod(lambda: None)
    def __init__(self):
        self.version = dict(BUILD)
        self.content = {'game.apk': b'apk', 'game data/main.12.test.game.obb': b'data', 'game data/required.bundle': b'bundle'}
        self.allowed, self.failure = True, False
    def me(self): return ACCOUNT, 'Example'
    def library(self, account): return [dict(app_id=APP, name='Example')] if self.allowed else []
    def latest(self, app): return dict(self.version)
    def build_files(self, build): return m.Client.files(binary(), build)
    def delivery_token(self): return 'private-fixture'
    def download(self, record, destination, token, progress):
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(self.content[record['path']])
        if self.failure: raise ValueError('Download interrupted')
        progress(record['size'])


class MetaLibraryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cache = m.LibraryStore(self.root, u.digest, u.write_json)
        self.client = FakeClient()
    def inspect(self, apk):
        return dict(import_id=u.digest(apk), version_code='12', package='test.game', meta_app_id=APP)
    def acquire(self):
        return self.cache.acquire(self.client, ACCOUNT, APP, self.inspect, lambda *args: None)
    def test_library_pagination_is_complete_and_deduplicated(self):
        client = m.Client('fixture')
        pages = [library_page([(APP, 'Game')], True, 'abc='), library_page([(APP, 'Game'), ('222', 'Second')])]
        with patch.object(client, 'query', side_effect=pages) as query:
            self.assertEqual(len(client.library(ACCOUNT)), 2)
            self.assertEqual(query.call_args_list[1].args[1]['cursor'], 'abc=')
        for malformed in ({}, {'data': {'node': {'app_entitlements': {'edges': [], 'page_info': {}}}}}, library_page([], True, None)):
            with self.assertRaises(ValueError): m.Client.library_page(malformed)
        with patch.object(client, 'query', return_value=library_page([], True, 'same')):
            with self.assertRaisesRegex(ValueError, 'pagination'): client.library(ACCOUNT)
    def test_all_assets_including_iap_and_edges_are_required(self):
        records = m.Client.files(binary(), '111')
        self.assertEqual(len(records), 3)
        self.assertEqual(records[-1]['path'], 'game data/required.bundle')
        assets = {'count': 1, 'edges': [{'node': {'id': '114', 'file_name': 'nested/bundle', 'size': '5'}}]}
        self.assertEqual(m.Client.files(binary(assets), '111')[-1]['path'], 'game data/nested/bundle')
        for assets in ({'count': 2, 'nodes': []}, {'nodes': [], 'page_info': {'has_next_page': True}}):
            with self.assertRaisesRegex(ValueError, 'part'): m.Client.files(binary(assets), '111')
    def test_current_build_details_resolve_unnamed_assets_from_delivery_headers(self):
        client = m.Client('profile-fixture')
        answer = binary({'count': 1, 'nodes': [{'id': '113', 'size': '6'}]})
        response = Response(b'x', 206, {'Content-Range': 'bytes 0-0/6',
                                      'Content-Disposition': 'attachment; filename="required.bundle"'})
        with patch.object(client, 'query', return_value=answer) as query, patch.object(client, 'delivery_token', return_value='delivery-fixture'), patch.object(client, 'open', return_value=response) as opened:
            files = client.build_files('111')
        self.assertEqual(query.call_args.args, ('24072064135771905', {'binaryID': '111'}))
        self.assertEqual(files[-1], dict(id='113', path='game data/required.bundle', size=6))
        self.assertEqual(opened.call_args.kwargs['headers'], {'Range': 'bytes=0-0'})
        self.assertEqual(opened.call_args.kwargs['token'], 'delivery-fixture')
        self.assertTrue(response.closed)
    def test_asset_headers_validate_filename_size_and_do_not_read_body(self):
        client = m.Client('profile-fixture')
        headers = {'Content-Length': '6', 'Content-Disposition': "attachment; filename*=UTF-8''required%20bundle"}
        response = Response(b'abcdef', 200, headers)
        with patch.object(response, 'read', side_effect=AssertionError('Metadata lookup must not read the asset body')), patch.object(client, 'delivery_token', return_value='delivery-fixture'), patch.object(client, 'open', return_value=response):
            self.assertEqual(client.asset_name('113', 6), 'required bundle')
        for status, headers, error in (
            (206, {'Content-Range': 'bytes 0-0/7'}, 'size'),
            (200, {'Content-Length': '7'}, 'size'),
            (206, {'Content-Range': 'bytes 0-0/6'}, 'filename'),
            (206, {'Content-Range': 'bytes 0-0/6', 'Content-Disposition': 'attachment; filename="../outside"'}, 'Unsafe')):
            with patch.object(client, 'delivery_token', return_value='delivery-fixture'), patch.object(client, 'open', return_value=Response(b'', status, headers)):
                with self.assertRaisesRegex(ValueError, error): client.asset_name('113', 6)
    def test_partial_asset_list_rejected_before_resolving_filenames(self):
        client = m.Client('profile-fixture')
        with patch.object(client, 'query', return_value=binary({'count': 2, 'nodes': [{'id': '113', 'size': 6}]})), patch.object(client, 'asset_name') as resolve:
            with self.assertRaisesRegex(ValueError, 'part'): client.build_files('111')
        resolve.assert_not_called()
    def test_http_error_identifies_operation_without_server_details(self):
        client = m.Client('profile-fixture')
        error = HTTPError('https://graph.oculus.com/graphql', 500, 'private-fixture', {}, None)
        with patch.object(client.opener, 'open', side_effect=error):
            with self.assertRaisesRegex(ValueError, r'Meta file lookup failed \(HTTP 500\)') as caught:
                client.build_files('111')
        self.assertNotIn('private-fixture', str(caught.exception))
        error.close()
    def test_delivery_session_is_reused_for_asset_metadata_and_downloads(self):
        client = m.Client('profile-fixture')
        with patch.object(client, 'graph', return_value={'access_token': 'delivery-fixture'}) as graph:
            self.assertEqual(client.delivery_token(), 'delivery-fixture')
            self.assertEqual(client.delivery_token(), 'delivery-fixture')
        graph.assert_called_once()
    def test_unsafe_and_colliding_names_rejected(self):
        for name in ('../outside', '/outside', 'C:\\outside', 'x/../../outside', 'x\x00y'):
            assets = {'nodes': [{'id': '114', 'file_name': name, 'size': '5'}]}
            with self.assertRaises(ValueError): m.Client.files(binary(assets), '111')
        assets = {'nodes': [{'id': '114', 'file_name': 'A.bundle', 'size': 1}, {'id': '115', 'file_name': 'a.bundle', 'size': 1}]}
        with self.assertRaisesRegex(ValueError, 'conflicting'): m.Client.files(binary(assets), '111')
    def test_refresh_failure_preserves_catalog_and_no_false_freshness(self):
        self.cache.refresh(self.client, lambda *args: None)
        before = (self.cache.account(ACCOUNT)/'catalog.json').read_bytes()
        with patch.object(self.client, 'library', side_effect=ValueError('Incomplete')):
            with self.assertRaises(ValueError): self.cache.refresh(self.client, lambda *args: None)
        self.assertEqual(before, (self.cache.account(ACCOUNT)/'catalog.json').read_bytes())
        self.assertFalse(self.cache.cached(ACCOUNT)['games'][0]['remote_known'])
        self.assertFalse(self.cache.cached(OTHER)['games'])
    def test_publish_use_corruption_and_account_isolation(self):
        info, receipt, catalog = self.acquire()
        self.assertTrue(receipt['valid'])
        self.assertTrue(catalog['games'][0]['remote_known'])
        self.assertTrue(self.cache.local(ACCOUNT, APP)['valid'])
        self.assertIsNone(self.cache.local(OTHER, APP))
        serialized = self.cache.pointer(ACCOUNT, APP).read_text()
        self.assertNotIn('private-fixture', serialized)
        self.assertNotIn('token', serialized)
        file = Path(receipt['folder'])/'game.apk'
        file.write_bytes(b'bad') # same-length corruption must be caught before use
        self.assertFalse(self.cache.local(ACCOUNT, APP)['valid'])
    def test_failed_update_and_version_mismatch_keep_previous_copy(self):
        _, previous, _ = self.acquire()
        self.client.failure = True
        with self.assertRaises(ValueError): self.acquire()
        self.assertEqual(self.cache.local(ACCOUNT, APP)['download_id'], previous['download_id'])
        self.client.failure = False
        self.client.version['version_code'] = 13
        with self.assertRaisesRegex(ValueError, 'version'): self.acquire()
        self.assertEqual(self.cache.local(ACCOUNT, APP)['download_id'], previous['download_id'])
    def test_ownership_and_account_rechecked_before_download(self):
        self.client.allowed = False
        with self.assertRaisesRegex(ValueError, 'access'): self.acquire()
        self.client.allowed = True
        with patch.object(self.client, 'me', return_value=(OTHER, 'Other')):
            with self.assertRaisesRegex(ValueError, 'account changed'): self.acquire()
        self.assertFalse(self.cache.pointer(ACCOUNT, APP).exists())
    def test_expired_entitlements_keep_download_visible(self):
        self.acquire()
        self.client.allowed = False
        catalog = self.cache.refresh(self.client, lambda *args: None)
        self.assertEqual(catalog['games'][0]['app_id'], APP)
        self.assertFalse(catalog['games'][0]['owned'])
        self.assertFalse(catalog['games'][0]['remote_known'])
        self.assertTrue(catalog['games'][0]['downloaded']['valid'])
    def test_delivery_resume_header_and_range_validation(self):
        path = self.root/'file'
        path.write_bytes(b'abc')
        record = dict(id='112', size=6)
        client = m.Client('profile-fixture')
        responses = [Response(b'def', 206, {'Content-Range': 'bytes 3-5/6', 'Content-Length': '3'})]
        requests = []
        class Opener:
            def open(self, request, timeout):
                requests.append(request)
                return responses.pop(0)
        client.opener = Opener()
        client.download(record, path, 'delivery-fixture', lambda size: None)
        self.assertEqual(path.read_bytes(), b'abcdef')
        self.assertEqual(requests[0].get_header('Range'), 'bytes=3-')
        self.assertEqual(requests[0].get_header('Authorization'), 'Bearer delivery-fixture')
        self.assertNotIn('fixture', requests[0].full_url)
        path.write_bytes(b'abc')
        responses.append(Response(b'def', 206, {'Content-Range': 'bytes 0-2/6'}))
        with self.assertRaisesRegex(ValueError, 'range'): client.download(record, path, 'delivery-fixture', lambda size: None)
        self.assertEqual(path.read_bytes(), b'abc')
        responses.append(Response(b'abcdef', 200, {'Content-Length': '6'}))
        client.download(record, path, 'delivery-fixture', lambda size: None)
        self.assertEqual(path.read_bytes(), b'abcdef')
    def test_cdn_redirect_strips_authorization_and_rejects_http(self):
        handler = m.SafeRedirect()
        req = Request('https://securecdn.oculus.com/binaries/download/?id=1', headers={'Authorization': 'Bearer fixture'})
        redirected = handler.redirect_request(req, None, 302, '', {}, 'https://example.fbcdn.net/signed')
        self.assertIsNone(redirected.get_header('Authorization'))
        for target in ('http://example.fbcdn.net/file', 'https://evil.example/file', 'https://user:password@example.fbcdn.net/file'):
            with self.assertRaises(ValueError): handler.redirect_request(req, None, 302, '', {}, target)
    def test_api_failures_redact_server_tokens_and_invalid_shapes(self):
        client = m.Client('private-fixture')
        for body in ([1, 2], {'error': {'code': 190, 'message': 'private-fixture'}}, {'errors': [{'message': 'private-fixture'}]}):
            with patch.object(client, 'open', return_value=Response(json.dumps(body).encode())):
                with self.assertRaises(ValueError) as error: client.graph('me')
            self.assertNotIn('private-fixture', str(error.exception))
        for url in ('https://evil.example/collect', 'http://graph.oculus.com/me', 'https://graph.oculus.com:444/me'):
            with self.assertRaisesRegex(ValueError, 'address'): client.open(url)
    def test_failed_catalog_write_and_cancel_preserve_previous_pointer(self):
        _, previous, _ = self.acquire()
        original_write = self.cache.write_json
        def fail_catalog(path, value):
            if Path(path).name == 'catalog.json': raise OSError('Fixture write failure')
            original_write(path, value)
        with patch.object(self.cache, 'write_json', side_effect=fail_catalog):
            with self.assertRaises(OSError): self.acquire()
        self.assertEqual(self.cache.local(ACCOUNT, APP)['download_id'], previous['download_id'])
        cancelled = [False]
        def check():
            if cancelled[0]: raise u.Cancelled('Cancelled')
        def inspect_then_cancel(apk):
            value = self.inspect(apk); cancelled[0] = True; return value
        with patch.object(self.client, 'check', side_effect=check):
            with self.assertRaises(u.Cancelled):
                self.cache.acquire(self.client, ACCOUNT, APP, inspect_then_cancel, lambda *args: None)
        self.assertEqual(self.cache.local(ACCOUNT, APP)['download_id'], previous['download_id'])
    def test_refresh_wrong_account_does_not_save_other_identity(self):
        with patch.object(self.client, 'me', return_value=(OTHER, 'Other')):
            with self.assertRaisesRegex(ValueError, 'account changed'):
                self.cache.refresh(self.client, lambda *args: None, expected_account=ACCOUNT)
        self.assertFalse((self.cache.account(OTHER)/'catalog.json').exists())

    def test_meta_source_does_not_gate_on_bundled_steam_sdk(self):
        info = dict(steam=True, host_tree=str(self.root), game_source='meta')
        with patch.object(u, 'steam_audit', return_value=False):
            self.assertEqual(u.resolve_steam('auto', info, None), 'none')
            self.assertEqual(u.resolve_steam('none', info, None), 'none')
            info['game_source'] = 'steam'
            self.assertEqual(u.resolve_steam('auto', info, None), 'offline')
            with self.assertRaises(u.UtilityError): u.resolve_steam('none', info, None)

    def test_native_identity_frozen_for_manifest_and_audited_local_apk(self):
        tree = self.root/'tree'; tree.mkdir()
        manifest = tree/'AndroidManifest.xml'
        manifest.write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android"><application><meta-data android:name="com.oculus.sdk.appid" android:value="123"/></application></manifest>')
        self.assertEqual(meta_game.app_id(tree), '123')
        with self.assertRaisesRegex(ValueError, 'does not match'): meta_game.app_id(tree, selected=APP)
        manifest.write_text('<manifest/>')
        self.assertEqual(meta_game.app_id(tree, selected=APP), APP)
        self.assertEqual(meta_game.app_id(tree), '')
        fingerprint = next(iter(meta_game.KNOWN_APPS))
        self.assertEqual(meta_game.app_id(tree, fingerprint), APP)
        with self.assertRaises(ValueError): meta_game.app_id(tree, selected='../token')
    def test_utility_use_verifies_then_attaches_all_data(self):
        _, receipt, _ = self.acquire()
        folder = Path(receipt['folder'])
        tree = self.root/'tree'; tree.mkdir(); (tree/'AndroidManifest.xml').write_text('<manifest/>')
        info = dict(self.inspect(folder/'game.apk'), host_tree=str(tree))
        class Runner:
            check_cancelled = staticmethod(lambda: None)
            start = staticmethod(lambda *args: None)
        request = io.StringIO(json.dumps(dict(account_id=ACCOUNT, app_id=APP))+'\n')
        events = []
        with patch.object(u, 'STORE', self.root), patch.object(u.sys, 'stdin', request), patch.object(u, 'inspect_apk', return_value=info), patch.object(u, 'emit', side_effect=lambda *args, **kw: events.append((args,kw))):
            u.meta_library_job('use', Runner())
        self.assertEqual(events[-1][0][0], 'meta-game')
        self.assertEqual(events[-1][1]['downloaded_data'], [str(folder/'game data')])
        self.assertEqual(events[-1][1]['meta_app_id'], APP)
        (folder/'game.apk').write_bytes(b'bad')
        with patch.object(u, 'STORE', self.root), patch.object(u.sys, 'stdin', io.StringIO(json.dumps(dict(account_id=ACCOUNT, app_id=APP))+'\n')), patch.object(u, 'emit', side_effect=lambda *args, **kw: events.append((args,kw))):
            with self.assertRaisesRegex(u.UtilityError, 'damaged'): u.meta_library_job('use', Runner())
        self.assertFalse(events[-1][1]['games'][0]['downloaded']['valid'])


if __name__ == '__main__': unittest.main()
