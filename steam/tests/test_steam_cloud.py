"""Exact-APK Android Cloud mappings and publisher metadata isolation."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('cloud_test_mksteam',ROOT/'steam/visionos/mksteam.py')
staging=importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)

class AndroidCloudConfigurationTests(unittest.TestCase):
    def test_pistolwhip_rules_require_the_exact_audited_apk(self):
        profile=json.loads((ROOT/'games/pistolwhip/steam/steam_api.lock.json').read_text())
        publisher=dict(enabled=True,rules=[dict(root='WinAppDataLocalLow',path='Cloudhead Games LTD/Pistol Whip',
            pattern='*.dat',recursive=False,platforms=['windows'])],overrides=[])
        result=staging.cloud_configuration(profile,publisher,profile['cloud_android_apk_sha256'])
        self.assertEqual(len(publisher['rules']),1)
        self.assertEqual(result['rules'][-1],profile['cloud_android_rules'][0])
        self.assertEqual(result['rules'][-1]['platforms'],['android'])
        self.assertEqual(result['preferences'],profile['cloud_android_preferences'])
        self.assertNotIn('preferences',publisher)
        self.assertEqual(staging.cloud_configuration(profile,publisher,'0'*64),publisher)
        disabled=dict(publisher,enabled=False)
        self.assertEqual(staging.cloud_configuration(profile,disabled,profile['cloud_android_apk_sha256']),disabled)
        unknown=staging.cloud_configuration(profile,None,profile['cloud_android_apk_sha256'])
        self.assertEqual(unknown['rules'],profile['cloud_android_rules'])
        self.assertIn('setenv("KL_UNITY_QUIT_PAUSE_ONLY", "1", 0)',staging.host_source('pistolwhip-12'))
        self.assertNotIn('setenv("KL_UNITY_QUIT_PAUSE_ONLY", "1", 0)',staging.host_source('eleven'))

if __name__=='__main__':unittest.main()
