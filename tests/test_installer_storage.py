"""Storage cleanup boundaries, recovery protection and offline UI contract."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import installer_storage as storage
import steam_library
import utility as u


class InstallerStorageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/'store'
        self.root.mkdir()

    def file(self, relative, content=b'12345'):
        path = self.root/relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def json(self, relative, value):
        return self.file(relative, json.dumps(value).encode())

    def download(self, service='steam', account='76561198000000001', identifier='a'*32):
        base = f'{service}-library/accounts/{account}'
        folder = self.file(f'{base}/downloads/{identifier}/game.apk').parent
        receipt = dict(download_id=identifier, app_id='10', name='Example VR', branch='beta',
                       build_id='101', downloaded_at='2026-11-15T12:00:00+00:00')
        pointer = self.json(f'{base}/apps/10/current.json' if service == 'steam' else f'{base}/apps/10.json', receipt)
        self.json(f'{base}/receipts/{identifier}.json', receipt)
        return folder, pointer

    def entry(self, path):
        return next(e for e in storage.inventory(self.root)['entries'] if e['path'] == str(path))

    def test_inventory_reports_current_old_and_partial_downloads_across_accounts(self):
        folder, _ = self.download()
        meta, _ = self.download('meta', '1234')
        old = self.file('steam-library/accounts/76561198000000001/downloads/'+'b'*32+'/game.apk', b'old').parent
        self.json('steam-library/accounts/76561198000000001/receipts/'+'b'*32+'.json',
                  dict(name='Previous VR', build_id='100', downloaded_at='2026-11-14T12:00:00Z'))
        partial = self.file('meta-library/accounts/1234/pending/10-version/game.apk.part', b'partial').parent
        report = storage.inventory(self.root)
        self.assertEqual(len(report['entries']), 4)
        self.assertEqual(report['removable_bytes'], 5+5+3+7)
        self.assertGreater(report['total_bytes'], report['removable_bytes'])
        current = self.entry(folder)
        self.assertEqual(current['name'], 'Example VR')
        self.assertIn('beta · Build 101', current['detail'])
        self.assertEqual(current['date'], '2026-11-15T12:00:00+00:00')
        self.assertEqual(self.entry(meta)['category'], 'Meta downloads')
        self.assertEqual(self.entry(old)['name'], 'Previous VR')
        self.assertIn('Older or unused', self.entry(old)['detail'])
        self.assertEqual(self.entry(partial)['category'], 'Interrupted downloads')

    def test_cleanup_removes_only_selected_copy_and_its_matching_pointers(self):
        folder, pointer = self.download()
        other, other_pointer = self.download('meta', '1234')
        original = Path(self.temp.name)/'original.apk'
        original.write_bytes(b'original')
        imported = self.file('imports/'+'f'*64+'/source.apk').parent
        self.json('imports/'+'f'*64+'/inspection.json', dict(name='Local game', source_path=str(original)))
        backup = self.file('jobs/123/backup/saves.dat')
        log = self.file('jobs/123/build.log')
        evidence = self.json('receipts/deployed', dict(bundle='com.test'))
        report = storage.cleanup(self.root, [self.entry(folder)['id'], self.entry(imported)['id']])
        self.assertEqual(report['removed_count'], 2)
        self.assertFalse(folder.exists())
        self.assertFalse(pointer.exists())
        for path in (original, other, other_pointer, backup, log, evidence):
            self.assertTrue(path.exists(), path)
        self.assertTrue(list((pointer.parents[2]/'receipts').glob('*.json')))

    def test_recovery_and_unreadable_recovery_record_protect_all_working_copies(self):
        folder, _ = self.download()
        imported = self.file('imports/fixture/source.apk').parent
        derived = self.file('jobs/fixture/DerivedData/app').parent
        self.file('pending/device-bundle', b'not-json')
        report = storage.inventory(self.root)
        self.assertEqual(report['removable_bytes'], 0)
        self.assertTrue(report['warnings'])
        for path in (folder, imported, derived):
            entry = self.entry(path)
            self.assertTrue(entry['protected'])
            with self.assertRaisesRegex(ValueError, 'protected'):
                storage.cleanup(self.root, [entry['id']])
            self.assertTrue(path.exists())

    def test_bad_library_receipt_protects_download_instead_of_treating_as_unused(self):
        folder, pointer = self.download()
        pointer.write_text('invalid')
        self.assertTrue(self.entry(folder)['protected'])
        self.assertTrue(storage.inventory(self.root)['warnings'])
        pointer.write_text(json.dumps(dict(download_id='../escape')))
        self.assertTrue(self.entry(folder)['protected'])

    def test_symlinks_do_not_scan_or_delete_external_files(self):
        external = Path(self.temp.name)/'external'
        external.mkdir()
        original = external/'valuable.apk'
        original.write_bytes(b'original')
        imports = self.root/'imports'
        imports.mkdir()
        (imports/'linked-folder').symlink_to(external, target_is_directory=True)
        real = self.file('imports/real/source.apk').parent
        (real/'linked-file').symlink_to(original)
        report = storage.inventory(self.root)
        self.assertEqual(len(report['entries']), 1)
        self.assertEqual(report['entries'][0]['bytes'], 5)
        storage.cleanup(self.root, [report['entries'][0]['id']])
        self.assertEqual(original.read_bytes(), b'original')
        self.assertTrue((imports/'linked-folder').is_symlink())
        (self.root/'meta-library').symlink_to(external, target_is_directory=True)
        self.assertEqual(storage.inventory(self.root)['entries'], [])

    def test_rejects_arbitrary_paths_and_stale_ids_before_deleting_anything(self):
        first = self.file('imports/first/source.apk').parent
        second = self.file('imports/second/source.apk').parent
        ids = [self.entry(first)['id'], self.entry(second)['id']]
        self.file('imports/second/new.apk')  # Changes directory identity since selection.
        with self.assertRaisesRegex(ValueError, 'Storage changed'):
            storage.cleanup(self.root, ids)
        self.assertTrue(first.exists())
        for identifiers in (['../../original.apk'], [str(first)], [], None, 'all', [1]):
            with self.subTest(identifiers=identifiers), self.assertRaises(ValueError):
                storage.cleanup(self.root, identifiers)
        self.assertTrue(first.exists())

    def test_replaced_directory_symlink_cannot_be_deleted_using_old_id(self):
        folder, _ = self.download()
        identifier = self.entry(folder)['id']
        import shutil
        shutil.rmtree(folder)
        external = Path(self.temp.name)/'external'
        external.mkdir()
        folder.symlink_to(external, target_is_directory=True)
        with self.assertRaises(ValueError):
            storage.cleanup(self.root, [identifier])
        self.assertTrue(external.exists())

    def test_changed_contents_in_existing_file_invalidate_selection(self):
        file = self.file('imports/first/source.apk')
        identifier = self.entry(file.parent)['id']
        file.write_bytes(b'changed payload')
        with self.assertRaisesRegex(ValueError, 'Storage changed'):
            storage.cleanup(self.root, [identifier])
        self.assertTrue(file.exists())

    def test_build_cleanup_keeps_backup_and_install_evidence(self):
        derived = self.file('jobs/123/DerivedData/app').parent
        payload = self.file('jobs/123/payload/assets').parent
        backup = self.file('jobs/123/backup/saved.dat')
        manifest = self.json('jobs/123/manifest.json', dict(import_id='test'))
        result = self.json('jobs/123/result.json', dict(installed=True))
        storage.cleanup(self.root, [self.entry(derived)['id'], self.entry(payload)['id']])
        self.assertFalse(derived.exists())
        self.assertFalse(payload.exists())
        for path in (backup, manifest, result):
            self.assertTrue(path.exists())

    def test_cli_scan_cleanup_contract_is_offline_and_does_not_create_jobs(self):
        folder, _ = self.download()
        output = io.StringIO()
        with patch.object(u, 'STORE', self.root), contextlib.redirect_stdout(output):
            self.assertEqual(u.main(['storage']), 0)
            entry = json.loads(output.getvalue())['entries'][0]
            output.seek(0); output.truncate()
            with patch.object(sys, 'stdin', io.StringIO(json.dumps(dict(ids=[entry['id']]))+'\n')):
                self.assertEqual(u.main(['storage', '--action', 'cleanup']), 0)
        event = json.loads(output.getvalue())
        self.assertEqual(event['stage'], 'storage')
        self.assertEqual(event['schema_version'], 1)
        self.assertEqual(event['removed_count'], 1)
        self.assertFalse(folder.exists())
        self.assertFalse((self.root/'jobs').exists())

    def test_cleanup_obeys_checkout_lock(self):
        folder, _ = self.download()
        output = io.StringIO()
        with patch.object(u, 'STORE', self.root), contextlib.redirect_stdout(output), u.checkout_lock():
            self.assertEqual(u.main(['storage']), 1)
        self.assertIn('Another APK Installer job', output.getvalue())
        self.assertTrue(folder.exists())

    def test_published_history_identifies_old_versions_and_cleanup_refreshes_local_status(self):
        cache = steam_library.LibraryStore(self.root, u.digest, u.write_json)
        account = '76561198000000001'
        launch = dict(id='0', name='Example', executable='game.apk')
        branch = dict(name='public', build_id='100', revision='first', updated_at=None,
                      available=True, locked=False, depots=[])
        game = dict(app_id='10', name='Example VR', android_available=True, vr=True,
                    branches=[branch], launches=[launch])

        def publish(content):
            pending = Path(self.temp.name)/('pending-'+content)
            pending.mkdir()
            (pending/'game.apk').write_text(content)
            (pending/'receipt.json').write_text('game-owned file')
            result = dict(steam_id=account, game=game, branch=branch, launch=launch,
                          files=[dict(path=p.name, size=p.stat().st_size, sha256=u.digest(p)) for p in pending.iterdir()])
            return cache.publish(result, pending, lambda _: dict(import_id='f'*64))

        _, _, old, _ = publish('old')
        branch.update(build_id='101', revision='second')
        _, _, current, _ = publish('new')
        self.assertEqual(self.entry(old)['name'], 'Example VR')
        self.assertIn('Build 100', self.entry(old)['detail'])
        self.assertIn('Older or unused', self.entry(old)['detail'])
        self.assertEqual((current/'receipt.json').read_text(), 'game-owned file')
        storage.cleanup(self.root, [self.entry(old)['id']])
        self.assertTrue(cache.local(account, '10', 'public', '0')['valid'])
        storage.cleanup(self.root, [self.entry(current)['id']])
        self.assertIsNone(cache.local(account, '10', 'public', '0'))
        self.assertEqual(cache.cached(account)['games'][0]['branches'][0]['downloaded'], {})

    def test_cli_rejects_non_object_request_without_a_traceback(self):
        output = io.StringIO()
        with patch.object(u, 'STORE', self.root), contextlib.redirect_stdout(output), \
             patch.object(sys, 'stdin', io.StringIO('[]\n')):
            self.assertEqual(u.main(['storage', '--action', 'cleanup']), 1)
        self.assertEqual(json.loads(output.getvalue())['status'], 'error')


if __name__ == '__main__':
    unittest.main()
