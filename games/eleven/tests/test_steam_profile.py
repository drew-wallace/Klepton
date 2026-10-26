"""Target separation for Steam ownership and SDK packaging."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('mksteam', ROOT/'steam/visionos/mksteam.py')
mksteam = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mksteam)


class SteamProfileTests(unittest.TestCase):
    def test_eleven_uses_its_supplied_sdk_and_app_context(self):
        config = mksteam.profile('eleven')
        self.assertEqual(config['app_id'], 488310)
        self.assertEqual(config['interfaces'][2], 'SteamApps009')
        self.assertEqual([p['path'] for p in config['files']], ['eleven/lib/arm64-v8a/libsteam_api.so'])
        source = mksteam.host_source('eleven')
        self.assertIn('setenv("KL_STEAM_APP_ID", "488310", 1)', source)
        self.assertIn('setenv("KL_STEAM_APPS_INTERFACE", "SteamApps009", 1)', source)
        self.assertIn('Steam is ready for Eleven Table Tennis.', source)
        self.assertNotIn('Preparing Walkabout', source)

    def test_walkabout_keeps_its_context_and_cpp_dependency(self):
        config = mksteam.profile()
        self.assertEqual(config['app_id'], 1408230)
        self.assertEqual(config['interfaces'][2], 'SteamApps008')
        self.assertEqual(len(config['files']), 2)
        self.assertIn('setenv("KL_STEAM_APP_ID", "1408230", 1)', mksteam.host_source())

    def test_staged_target_cannot_be_reused_for_another_game(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(mksteam, 'OUT', Path(tmp)), \
                patch.object(mksteam, 'inputs', return_value=(None, [])):
            (Path(tmp)/'manifest.json').write_text(json.dumps({
                'target': 'walkabout-57013', 'profile': mksteam.profile(),
                'platform': 'visionossim', 'inputs': [], 'artifacts': [],
            }))
            with self.assertRaisesRegex(ValueError, 'another target'):
                mksteam.verify_staged('eleven')

    def test_unknown_target_refused(self):
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            mksteam.profile('unknown')


if __name__ == '__main__':
    unittest.main()
