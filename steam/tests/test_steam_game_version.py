"""Installed version provenance and generated app packaging; no game required."""
import importlib.util
import plistlib
from pathlib import Path
import tempfile
import types
import unittest
import subprocess
from datetime import datetime
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('game_version', ROOT/'steam/visionos/game_version.py')
versions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(versions)


class GameVersionTests(unittest.TestCase):
    def test_explicit_apk_dates_and_download_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = root/'AndroidManifest.xml'
            with patch.object(versions, 'downloaded_date', return_value='2026-09-25T22:06:25+00:00') as download:
                manifest.write_text('''<manifest xmlns:android="http://schemas.android.com/apk/res/android"
                    platformBuildVersionCode="34" platformBuildVersionName="14"><application>
                    <meta-data android:name="releaseDate" android:value="2026-09-21"/>
                    <meta-data android:name="buildDate" android:value="2026-09-20T18:15:00Z"/>
                    </application></manifest>''')
                self.assertEqual(versions.installed_version(root), {
                    'KleptonGameReleaseDate': '2026-09-21',
                    'KleptonGameBuildDate': '2026-09-20T18:15:00+00:00'})
                download.assert_not_called()
                manifest.write_text('''<manifest xmlns:android="http://schemas.android.com/apk/res/android">
                    <application><meta-data android:name="buildDate" android:value="@string/date"/>
                    </application></manifest>''')
                self.assertEqual(versions.installed_version(root), {
                    'KleptonGameDownloadDate': '2026-09-25T22:06:25+00:00'})
                manifest.write_text('<invalid')
                self.assertIn('KleptonGameDownloadDate', versions.installed_version(root))

    def test_download_metadata_uses_real_apk_and_never_file_mtime(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            apk = root/'source.apk'
            apk.write_bytes(b'fixture')
            link = root/'linked.apk'
            link.symlink_to(apk)
            spotlight = plistlib.dumps({'kMDItemDownloadedDate': [datetime(2026, 9, 25)]})
            with patch.object(versions.subprocess, 'run', return_value=types.SimpleNamespace(stdout=spotlight)) as run:
                self.assertEqual(versions.downloaded_date(link), '2026-09-25T00:00:00+00:00')
                self.assertEqual(run.call_args.args[0][-1], str(apk.resolve()))
            with patch.object(versions.subprocess, 'run', side_effect=[
                    types.SimpleNamespace(stdout=plistlib.dumps({})),
                    types.SimpleNamespace(stdout=b'0082;6ab6f061;Nextcloud;')]):
                self.assertEqual(versions.downloaded_date(link), '2026-09-25T22:06:25+00:00')
            with patch.object(versions.subprocess, 'run', side_effect=subprocess.TimeoutExpired('metadata', 3)):
                self.assertIsNone(versions.downloaded_date(link))
            with patch.object(versions.subprocess, 'run', side_effect=[
                    types.SimpleNamespace(stdout=b'invalid'),
                    types.SimpleNamespace(stdout=b'0082;no timestamp;client;')]):
                self.assertIsNone(versions.downloaded_date(link))
            self.assertIsNone(versions.normalized_date('1980-01-01'))
            self.assertIsNone(versions.normalized_date('garbage'))

    def test_apktool_versions_and_missing_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertEqual(versions.installed_version(root), {})
            metadata = root/'apktool.yml'
            metadata.write_text('version: 3.0.3\nversionInfo:\n  versionCode: 57013\n  versionName: 6.7\nother: 1\n')
            self.assertEqual(versions.installed_version(root), {
                'KleptonGameVersion': '6.7', 'KleptonGameVersionCode': '57013'})
            metadata.write_text('versionInfo:\n  versionName: "6.7 beta"\n  versionCode: \'57013\'\n')
            self.assertEqual(versions.installed_version(root)['KleptonGameVersion'], '6.7 beta')
            metadata.write_text('versionInfo:\n  versionName: \'Player\'\'s build\'\n  versionCode: nope\n')
            self.assertEqual(versions.installed_version(root), {'KleptonGameVersion': "Player's build"})
            metadata.write_text('versionInfo:\n  versionName: \n  versionCode: 57013\n')
            self.assertEqual(versions.installed_version(root), {'KleptonGameVersionCode': '57013'})
            metadata.write_text('versionInfo:\n  versionName: null\n  versionCode: null\n')
            self.assertEqual(versions.installed_version(root), {})

    def test_steam_project_freezes_version_in_its_own_plist(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            metadata = root/'apktool.yml'
            metadata.write_text('versionInfo:\n  versionCode: 57013\n  versionName: 6.7\n')
            (root/'AndroidManifest.xml').write_text('''<manifest xmlns:android="http://schemas.android.com/apk/res/android">
                <application><meta-data android:name="build_date" android:value="2026-09-21"/></application></manifest>''')
            target = dict(name='walkabout-57013', product='KleptonWalkabout57013',
                          bundle='test.klepton.walkabout', display='Walkabout',
                          libs='libmain', tree=str(root), steam_local=True)
            with patch.dict('os.environ', {'KLEPTON_STEAM_LOCAL': '1',
                            'KLEPTON_ENTITLEMENTS': '0', 'KLEPTON_TEAM': 'TESTTEAM00'}), \
                 patch.dict('sys.modules', {
                     'targets': types.SimpleNamespace(DEFAULT=target['name'], resolve=lambda _: target),
                     'mksteam': types.SimpleNamespace(verify_staged=lambda: None),
                     'game_version': versions,
                 }):
                project_spec = importlib.util.spec_from_file_location('fixture_version_project', ROOT/'visionos/gen_xcodeproj.py')
                project = importlib.util.module_from_spec(project_spec)
                project_spec.loader.exec_module(project)
            plist = ROOT/'visionos'/project.GAME_INFO_PLIST
            installed = plistlib.loads(plist.read_bytes())
            self.assertEqual(installed['KleptonGameVersion'], '6.7')
            self.assertEqual(installed['KleptonGameVersionCode'], '57013')
            self.assertEqual(installed['KleptonGameBuildDate'], '2026-09-21')
            self.assertIn('UIApplicationSceneManifest', installed)
            self.assertIn(f'INFOPLIST_FILE = "{project.GAME_INFO_PLIST}";', project.COMMON)
            metadata.write_text('versionInfo:\n  versionName: 7.0\n')
            self.assertEqual(plistlib.loads(plist.read_bytes())['KleptonGameVersion'], '6.7')


if __name__ == '__main__':
    unittest.main()
