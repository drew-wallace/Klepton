"""Catalog-based external game data, without proprietary game fixtures."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import utility as u


class CatalogDataTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root/'game.apk'
        self.source.write_bytes(b'apk')
        self.obb = self.root/'main.42.test.example.obb'
        self.prefix = '/sdcard/Android/obb/test.example/'
        self.info = dict(package='test.example', version_code='42', kind='unity', entry='libmain',
                         libraries=['libmain'], host_tree=str(self.root), apk_path=str(self.source),
                         import_id='a'*64, name='Example', steam=False)
        self.request = dict(schema_version=1, team='ABCDEFGHIJ', device='fixture', data=[str(self.root)])
        self.write_catalog(['required.bundle', 'nested/asset.bundle'])
        (self.root/'required.bundle').write_bytes(b'one')
        (self.root/'nested').mkdir()
        (self.root/'nested/asset.bundle').write_bytes(b'two')
        for name in ('unreferenced.bundle', 'symbols.zip', 'PresetAvatars_Rift.zip', '.DS_Store', 'save.dat'):
            (self.root/name).write_bytes(b'ignored')

    def write_catalog(self, names):
        with zipfile.ZipFile(self.obb, 'w') as archive:
            archive.writestr('assets/aa/catalog.json', json.dumps({'m_InternalIds': [self.prefix+n for n in names]}))
            archive.writestr('assets/aa/Android/catalog.json', json.dumps({'m_InternalIds': [self.prefix+n for n in names]}))

    def inspected_info(self):
        catalog = u.catalog_game_data(self.source, self.info)
        return dict(self.info, catalog_data=catalog, suggested_data=[catalog['source_folder']],
                    suggested_data_path=catalog['destination'])

    def test_catalog_selects_only_required_files_and_maps_obb_and_bundles_together(self):
        info = self.inspected_info()
        self.assertEqual(info['catalog_data']['files'], [self.obb.name, 'nested/asset.bundle', 'required.bundle'])
        manifest = u.validate_request(self.request, info, self.root/'job')
        self.assertEqual(manifest['data_path'], 'Android/obb/test.example')
        self.assertEqual(manifest['obb_path'], manifest['data_path'])
        payload = u.prepare_payload(manifest, self.root/'job', [])
        self.assertEqual(set(payload), {'Documents/android-files/Android/obb/test.example/'+name
                                       for name in info['catalog_data']['files']})
        self.assertEqual(sum(p.stat().st_size for p in payload.values()), self.obb.stat().st_size+6)

    def test_missing_bundle_blocks_build_even_with_obb_selected(self):
        info = self.inspected_info()
        (self.root/'required.bundle').unlink()
        with self.assertRaisesRegex(u.UtilityError, 'Missing 1 catalog-referenced.*required.bundle'):
            u.validate_request(self.request, info, self.root/'job')
        with self.assertRaisesRegex(u.UtilityError, 'catalog-referenced'):
            u.validate_request(dict(self.request, data=[str(self.obb)]), info, self.root/'job')

    def test_dedicated_game_data_folder_is_discovered_and_filtered(self):
        data = self.root/'game data'
        data.mkdir()
        for name in (self.obb.name, 'required.bundle', 'nested'):
            (self.root/name).rename(data/name)
        (data/'symbols.zip').write_bytes(b'ignored')
        info = self.inspected_info()
        self.assertEqual(info['suggested_data'],[str(data)])
        manifest = u.validate_request(dict(self.request,data=[str(data)]),info,self.root/'job')
        payload = u.prepare_payload(manifest,self.root/'job',[])
        self.assertEqual(set(payload),{'Documents/android-files/Android/obb/test.example/'+name
                                      for name in info['catalog_data']['files']})
        self.assertEqual(len(payload),3)

    def test_payload_rechecks_files_after_validation(self):
        manifest = u.validate_request(self.request, self.inspected_info(), self.root/'job')
        (self.root/'required.bundle').unlink()
        with self.assertRaisesRegex(u.UtilityError, 'changed after validation'):
            u.prepare_payload(manifest, self.root/'job', [])

    def test_only_matching_version_and_package_obb_are_read(self):
        for info in (dict(self.info, package='other.package'), dict(self.info, version_code='43')):
            self.assertEqual(u.catalog_game_data(self.source, info), {})
        self.obb.write_bytes(b'non-ZIP expansion file')
        self.assertEqual(u.catalog_game_data(self.source, self.info), {})

    def test_unsafe_or_unrelated_catalog_paths_are_not_imported(self):
        self.write_catalog(['../escape.bundle', '/absolute.bundle', 'nested/../../escape.bundle',
                            'foo//bar.bundle', 'foo/./bar.bundle', 'bad\\path.bundle', 'symbols.zip', 'required.bundle'])
        with zipfile.ZipFile(self.obb, 'a') as archive:
            archive.writestr('assets/aa/catalog_other.json', json.dumps({'m_InternalIds': [None, 42,
                '/sdcard/Android/obb/other.package/wrong.bundle', 'http://localhost/remote.bundle']}))
        self.assertEqual(u.catalog_game_data(self.source, self.info)['files'], [self.obb.name, 'required.bundle'])

    def test_missing_obb_is_required_even_if_bundles_exist(self):
        info = self.inspected_info()
        self.obb.unlink()
        with self.assertRaisesRegex(u.UtilityError, 'Missing required game data: main.42'):
            u.validate_request(self.request, info, self.root/'job')

    def test_inspection_offers_one_filtered_folder(self):
        store = self.root/'store'
        store.mkdir()
        def decode(snapshot, tree, runner):
            tree.mkdir()
        with patch.object(u, 'STORE', store), patch.object(u, 'archive_inventory', return_value=(['libmain'], 0)), \
                patch.object(u, 'decode_import', side_effect=decode), patch.object(u, 'decoded_metadata', return_value=self.info.copy()), \
                patch.object(u.targets, 'libs_for', return_value='libmain'), patch.object(u, 'match_profiles', return_value=[]), \
                patch.object(u, 'emit'):
            info = u.inspect_apk(self.source, u.Runner(self.root/'inspection-job'))
        self.assertEqual(info['suggested_data'], [str(self.root)])
        self.assertEqual(info['suggested_data_path'], 'Android/obb/test.example')
        self.assertTrue(info['data_requirements']['']['required'])
        self.assertIn('2 catalog-referenced bundles', info['data_requirements']['']['description'])


if __name__ == '__main__':
    unittest.main()
