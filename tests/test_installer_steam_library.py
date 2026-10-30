"""Steam acquisition receipts, failure isolation, and installer pipe integration."""
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import shlex
import shutil
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import utility as u
import steam_library as library

ACCOUNT = '76561198000000001'
OTHER_ACCOUNT = '76561198000000002'


class SteamLibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root/'store'
        self.store.mkdir()
        self.cache = library.LibraryStore(self.store, u.digest, u.write_json)
        self.launch = dict(id='0', name='Example', executable='game.apk')
        self.branch = dict(name='public', build_id='100', updated_at=None, revision='first',
                           available=True, locked=False, depots=[dict(depot_id='10', manifest_id='18446744073709551614')])
        self.game = dict(app_id='10', name='Example VR', vr=True, android_available=True,
                         launches=[self.launch], branches=[self.branch])
        self.result = dict(steam_id=ACCOUNT, game=self.game, branch=self.branch, launch=self.launch)

    def pending(self, content=b'first'):
        pending = self.root/next(tempfile._get_candidate_names())
        pending.mkdir()
        (pending/'game.apk').write_bytes(content)
        (pending/'obb').mkdir()
        (pending/'obb/content.obb').write_bytes(b'assets')
        (pending/'loose.bundle').write_bytes(b'map me')
        result = copy.deepcopy(self.result)
        result['files'] = [dict(path=str(p.relative_to(pending)), size=p.stat().st_size, sha256=u.digest(p))
                           for p in sorted(pending.rglob('*')) if p.is_file()]
        return pending, result

    def inspect(self, apk):
        return dict(import_id=u.digest(apk), package='test.example', name='Example', kind='unity', source_path=str(apk))

    def publish(self, content=b'first'):
        pending, result = self.pending(content)
        return self.cache.publish(result, pending, self.inspect)

    def test_download_receipt_dates_and_cached_reuse_are_persistent(self):
        info, receipt, folder, catalog = self.publish()
        self.assertEqual(receipt['import_id'], info['import_id'])
        self.assertEqual(receipt['depots'][0]['manifest_id'], '18446744073709551614')
        self.assertTrue((folder/'game.apk').is_file())
        for _ in range(2):
            local = self.cache.local(ACCOUNT, '10', 'public', '0')
            self.assertTrue(local['valid'])
            self.assertEqual(local['downloaded_at'], receipt['downloaded_at'])
        self.assertEqual(catalog['games'][0]['branches'][0]['downloaded']['0']['build_id'], '100')
        self.assertTrue(catalog['games'][0]['branches'][0]['remote_known'])
        self.assertFalse(self.cache.cached(ACCOUNT)['games'][0]['branches'][0]['remote_known'])

    def test_update_keeps_original_content_and_publishes_new_revision(self):
        _, original, folder, _ = self.publish()
        self.branch.update(build_id='101', revision='second')
        _, updated, new_folder, _ = self.publish(b'second')
        self.assertNotEqual(folder, new_folder)
        self.assertEqual((folder/'game.apk').read_bytes(), b'first')
        self.assertEqual((new_folder/'game.apk').read_bytes(), b'second')
        self.assertEqual(self.cache.local(ACCOUNT, '10', 'public', '0')['revision'], 'second')

    def test_inspect_or_hash_failure_never_replaces_previous_receipt(self):
        _, original, _, _ = self.publish()
        pending, result = self.pending(b'update')
        def fail(_): raise ValueError('unsupported APK')
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            self.cache.publish(result, pending, fail)
        (pending/'game.apk').write_bytes(b'broken')
        with self.assertRaisesRegex(ValueError, 'verification'):
            self.cache.publish(result, pending, self.inspect)
        self.assertEqual(self.cache.local(ACCOUNT, '10', 'public', '0')['download_id'], original['download_id'])

    def test_cancellation_during_inspection_preserves_completed_copy(self):
        _, original, _, _ = self.publish()
        pending, result = self.pending(b'update')
        def cancel(_): raise u.Cancelled('Cancelled')
        with self.assertRaises(u.Cancelled):
            self.cache.publish(result, pending, cancel)
        self.assertEqual(self.cache.local(ACCOUNT, '10', 'public', '0')['download_id'], original['download_id'])

    def test_corrupted_or_missing_files_offer_repair(self):
        _, _, folder, _ = self.publish()
        (folder/'game.apk').write_bytes(b'other')  # Same byte count: hashes must also be checked.
        self.assertFalse(self.cache.local(ACCOUNT, '10', 'public', '0')['valid'])
        (folder/'game.apk').unlink()
        self.assertFalse(self.cache.local(ACCOUNT, '10', 'public', '0')['valid'])

    def test_account_branch_and_launch_are_isolated(self):
        self.publish()
        self.assertIsNone(self.cache.local(OTHER_ACCOUNT, '10', 'public', '0'))
        self.assertIsNone(self.cache.local(ACCOUNT, '10', 'beta', '0'))
        self.assertIsNone(self.cache.local(ACCOUNT, '10', 'public', '1'))
        self.assertNotEqual(self.cache.pointer(ACCOUNT, '10', '../beta', '0'), self.cache.pointer(ACCOUNT, '10', 'beta', '0'))
        with self.assertRaises(ValueError): self.cache.account('../escape')

    def test_payload_path_and_symlink_rejection(self):
        for value in ('../escape', '/abs', 'C:\\abs', 'a/../b', 'a//b', '\x00', '.'):
            with self.subTest(value=value), self.assertRaises(ValueError): library.safe_path(value)
        pending, result = self.pending()
        (pending/'game.apk').unlink()
        (pending/'game.apk').symlink_to(pending/'loose.bundle')
        with self.assertRaisesRegex(ValueError, 'verification'):
            self.cache.publish(result, pending, self.inspect)

    def test_data_mapping_attaches_obbs_and_exposes_unresolved_files(self):
        _, receipt, folder, _ = self.publish()
        attached, unresolved = library.data_candidates(folder, receipt)
        self.assertEqual(attached, [str(folder/'obb')])
        self.assertEqual(unresolved, ['loose.bundle'])

    def test_downloaded_ue4_data_uses_package_obb_destination(self):
        _, receipt, folder, _ = self.publish()
        info = dict(self.inspect(folder/'game.apk'), kind='ue4')
        with patch.object(u, 'STORE', self.store):
            u.steam_game_data(info, folder, receipt)
        self.assertEqual(info['suggested_data_path'], 'Android/obb/test.example')

    def test_backend_download_hands_inspection_and_obb_data_to_ui(self):
        def helper(root, request, runner, emit):
            pending, result = self.pending()
            destination = Path(request['destination'])
            import shutil
            shutil.copytree(pending, destination, dirs_exist_ok=True)
            return result
        output = io.StringIO()
        runner = u.Runner(self.root/'job')
        private_request = dict(steam_id=ACCOUNT, refresh_token='private-token', password='private-password',
                               app_id='10', branch='public', launch_id='0')
        with patch.object(u, 'STORE', self.store), patch.object(library, 'run_helper', side_effect=helper), \
             patch.object(u, 'inspect_apk', side_effect=lambda apk, runner: self.inspect(apk)), \
             patch.object(sys, 'stdin', io.StringIO(json.dumps(private_request)+'\n')), contextlib.redirect_stdout(output):
            u.library_job('download', runner)
        events = [json.loads(line) for line in output.getvalue().splitlines()]
        inspected = next(e for e in events if e['stage'] == 'steam-game')
        self.assertTrue(Path(inspected['source_path']).is_file())
        self.assertEqual(inspected['unmapped_data'], ['loose.bundle'])
        self.assertTrue(Path(inspected['downloaded_data'][0]).is_dir())
        self.assertEqual(inspected['suggested_data_path'], 'obb')
        for path in self.store.rglob('*.json'):
            self.assertNotIn('private-', path.read_text())
        self.assertNotIn('private-', output.getvalue())

    def test_use_cached_does_not_start_network_or_change_date(self):
        _, receipt, _, _ = self.publish()
        output = io.StringIO()
        request = dict(steam_id=ACCOUNT, app_id='10', branch='public', launch_id='0')
        with patch.object(u, 'STORE', self.store), patch.object(library, 'run_helper') as helper, \
             patch.object(u, 'inspect_apk', side_effect=lambda apk, runner: self.inspect(apk)), \
             patch.object(sys, 'stdin', io.StringIO(json.dumps(request)+'\n')), contextlib.redirect_stdout(output):
            u.library_job('use', u.Runner(self.root/'job'))
        helper.assert_not_called()
        self.assertEqual(self.cache.local(ACCOUNT, '10', 'public', '0')['downloaded_at'], receipt['downloaded_at'])
        self.assertIn('steam-game', output.getvalue())
        event = next(json.loads(line) for line in output.getvalue().splitlines() if json.loads(line)['stage'] == 'steam-game')
        self.assertEqual(event['suggested_data_path'], 'obb')
        saved = json.loads((self.store/'imports'/receipt['import_id']/'inspection.json').read_text())
        self.assertEqual(saved['suggested_data_path'], 'obb')

    def test_failed_remote_refresh_marks_old_catalog_unknown(self):
        self.publish()
        output = io.StringIO()
        with patch.object(u, 'STORE', self.store), patch.object(library, 'run_helper', side_effect=ValueError('Steam unavailable')), \
             patch.object(sys, 'stdin', io.StringIO(json.dumps(dict(steam_id=ACCOUNT))+'\n')), contextlib.redirect_stdout(output):
            with self.assertRaises(ValueError): u.library_job('refresh', u.Runner(self.root/'job'))
        event = next(json.loads(line) for line in output.getvalue().splitlines() if json.loads(line)['status'] == 'result')
        self.assertFalse(event['games'][0]['branches'][0]['remote_known'])
        self.assertTrue(event['games'][0]['branches'][0]['downloaded']['0']['valid'])

    def test_scoped_refresh_shortlists_vr_android_and_retains_unchecked_games(self):
        self.publish()
        desktop = dict(copy.deepcopy(self.game), app_id='11', vr=False, android_available=False)
        removed = dict(copy.deepcopy(self.game), app_id='99')
        self.cache.save_catalog(dict(steam_id=ACCOUNT, games=[self.game, desktop, removed],
                                    owned_app_ids=['10', '11', '99']))
        updated = copy.deepcopy(self.game)
        updated['branches'][0]['revision'] = 'second'
        result = dict(steam_id=ACCOUNT, games=[updated], checked_app_ids=['10'], owned_app_ids=['10', '11'])
        output = io.StringIO()
        request = dict(steam_id=ACCOUNT, scope='vr_android', refresh_token='fixture')
        with patch.object(u, 'STORE', self.store), patch.object(library, 'run_helper', return_value=result) as helper, \
             patch.object(sys, 'stdin', io.StringIO(json.dumps(request)+'\n')), contextlib.redirect_stdout(output):
            u.library_job('refresh', u.Runner(self.root/'job'))
        sent = helper.call_args.args[1]
        self.assertEqual(sent['app_ids'], ['10', '99'])
        self.assertEqual(sent['known_app_ids'], ['10', '11', '99'])
        event = [json.loads(line) for line in output.getvalue().splitlines() if json.loads(line)['status'] == 'result'][-1]
        games = {g['app_id']: g for g in event['games']}
        self.assertEqual(set(games), {'10', '11'})
        self.assertTrue(games['10']['branches'][0]['remote_known'])
        self.assertFalse(games['11']['branches'][0]['remote_known'])
        self.assertEqual(games['10']['branches'][0]['revision'], 'second')
        self.assertTrue(games['10']['branches'][0]['downloaded']['0']['valid'])
        self.assertEqual(self.cache.cached(ACCOUNT)['owned_app_ids'], ['10', '11'])

    def test_full_refresh_does_not_send_a_shortlist(self):
        self.cache.save_catalog(dict(steam_id=ACCOUNT, games=[self.game]))
        output = io.StringIO()
        request = dict(steam_id=ACCOUNT, scope='all')
        with patch.object(u, 'STORE', self.store), \
             patch.object(library, 'run_helper', return_value=dict(steam_id=ACCOUNT, games=[self.game])) as helper, \
             patch.object(sys, 'stdin', io.StringIO(json.dumps(request)+'\n')), contextlib.redirect_stdout(output):
            u.library_job('refresh', u.Runner(self.root/'job'))
        self.assertNotIn('app_ids', helper.call_args.args[1])
        self.assertNotIn('known_app_ids', helper.call_args.args[1])

    def start_bridge(self, scenario=''):
        root = Path(__file__).resolve().parents[1]
        node = shutil.which('node')
        if not node: self.skipTest('Node is not installed')
        wrapper = self.root/'node-fixture'
        wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(node)+' --require '+
                           shlex.quote(str(root/'steam/tools/library/fixtures/fake_steam.cjs'))+' "$@"\n')
        wrapper.chmod(0o700)
        script = 'import sys; from pathlib import Path; sys.path.insert(0, "tools"); import utility; utility.STORE=Path(sys.argv[1]); sys.exit(utility.main(["steam-library", "--action", "login"]))'
        child = subprocess.Popen([sys.executable, '-c', script, str(self.store)], cwd=root,
                                 env=dict(os.environ, KLEPTON_NODE=str(wrapper), KLEPTON_TEST_SCENARIO=scenario),
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: child.poll() is None and child.kill())
        return child

    def test_real_pipe_bridge_handles_guard_and_never_logs_credentials(self):
        child = self.start_bridge()
        child.stdin.write(json.dumps(dict(auth_method='password', account_name='fixture', password='secret-password'))+'\n')
        child.stdin.flush()
        events = []
        for line in child.stdout:
            event = json.loads(line)
            events.append(event)
            if event['status'] == 'auth':
                child.stdin.write(json.dumps(dict(guard_code='12345'))+'\n')
                child.stdin.flush()
        _, errors = child.communicate(timeout=10)
        self.assertEqual(child.returncode, 0, errors)
        self.assertTrue(any(e['status'] == 'credential' for e in events))
        self.assertTrue(any(e['status'] == 'result' and e['stage'] == 'steam-library' for e in events))
        for event in events:
            if event['status'] != 'credential': self.assertNotIn('private-fixture', json.dumps(event))
        for file in self.store.rglob('*'):
            if file.is_file():
                self.assertNotIn('secret-password', file.read_text())
                self.assertNotIn('private-fixture', file.read_text())

    def test_real_pipe_bridge_preserves_partial_refresh_metadata(self):
        desktop = dict(copy.deepcopy(self.game), app_id='11', vr=False, android_available=False)
        self.cache.save_catalog(dict(steam_id=ACCOUNT, games=[self.game, desktop], owned_app_ids=['10', '11']))
        child = self.start_bridge('scoped')
        output, errors = child.communicate(json.dumps(dict(steam_id=ACCOUNT, refresh_token='fixture', scope='vr_android'))+'\n', timeout=10)
        self.assertEqual(child.returncode, 0, errors)
        event = next(json.loads(line) for line in output.splitlines()
                     if json.loads(line)['status'] == 'result' and json.loads(line)['stage'] == 'steam-library')
        games = {g['app_id']: g for g in event['games']}
        self.assertEqual(set(games), {'10', '11', '12'})
        self.assertTrue(games['10']['branches'][0]['remote_known'])
        self.assertFalse(games['11']['branches'][0]['remote_known'])
        self.assertTrue(games['12']['branches'][0]['remote_known'])
        self.assertEqual(self.cache.cached(ACCOUNT)['owned_app_ids'], ['10', '11', '12'])

    def test_cancellation_stops_pending_authentication_and_reports_cancelled(self):
        child = self.start_bridge('waiting')
        child.stdin.write(json.dumps(dict(auth_method='password', account_name='fixture', password='secret-password'))+'\n')
        child.stdin.flush()
        for line in child.stdout:
            if json.loads(line)['status'] == 'auth': break
        child.terminate()
        output, errors = child.communicate(timeout=10)
        self.assertEqual(child.returncode, 130, errors)
        self.assertIn('"status": "cancelled"', output)
        self.assertNotIn('secret-password', output)


if __name__ == '__main__': unittest.main()
