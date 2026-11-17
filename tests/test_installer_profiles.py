"""Verified game defaults must work on a fresh install, without device settings."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import utility as u


class ProfileDefaultsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tree = self.root / 'decoded'
        (self.tree / 'lib/arm64-v8a').mkdir(parents=True)
        elf = bytearray(64)
        elf[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', elf, 18, 183)
        for name in ('libmain', 'libunity'):
            (self.tree / 'lib/arm64-v8a' / (name + '.so')).write_bytes(elf)
        self.record = u.profile_catalog()['pistolwhip-12']
        manifest = ('<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                    'package="com.cloudheadgames.pistolwhip"><application android:label="Pistol Whip"/></manifest>')
        (self.tree / 'AndroidManifest.xml').write_text(manifest)
        (self.tree / 'apktool.yml').write_text("versionCode: '12'\nversionName: '1.6.0.2'\n")
        self.apk = self.root / 'Pistol Whip.apk'
        with zipfile.ZipFile(self.apk, 'w') as z:
            z.writestr('AndroidManifest.xml', manifest)
            for name in ('libmain', 'libunity'):
                z.writestr(f'lib/arm64-v8a/{name}.so', elf)
        self.info = dict(package=self.record['package'], version_code='12', name='Pistol Whip',
                         kind='unity', entry='libmain', libraries=['libmain', 'libunity'],
                         entry_libraries=['libmain', 'libunity'], steam=True, obb_required=False,
                         apk_sha256=self.record['apk_sha256'], import_id=self.record['apk_sha256'],
                         apk_path=str(self.apk), host_tree=str(self.tree))
        self.request = dict(schema_version=1, profile='', steam_mode='auto',
                            team='ABCDEFGHIJ', device='fixture-device', data=[])

    def resolve(self, manifest):
        path = self.root / 'manifest.json'
        path.write_text(json.dumps(manifest))
        with patch.object(u.targets, 'libs_for', return_value='libmain libunity'):
            return u.targets.resolve_manifest(path, manifest['target'])

    def test_verified_build_matches_without_local_reference_game_tree(self):
        self.assertEqual(u.match_profiles(self.info, self.tree), ['pistolwhip-12'])
        self.assertIn('2048-pixel', u.targets.TARGETS['pistolwhip-12']['compatibility_note'])

    def test_fresh_inspection_selects_profile_and_carries_defaults_into_generated_app(self):
        store = self.root / 'store'
        store.mkdir()
        record = dict(self.record, apk_sha256=u.digest(self.apk))
        fixture = self.tree
        class Decoder(u.Runner):
            def run(self, args, env=None, timeout=None):
                shutil.copytree(fixture, Path(args[args.index('-o') + 1]))
        with (patch.object(u, 'STORE', store),
              patch.object(u, 'profile_catalog', return_value={'pistolwhip-12': record}),
              patch.object(u.shutil, 'which', return_value='fixture-apktool'),
              patch.object(u.targets, 'libs_for', return_value='libmain libunity'),
              contextlib.redirect_stdout(io.StringIO())):
            info = u.inspect_apk(self.apk, Decoder(self.root / 'inspect'))
            self.assertEqual(info['profile'], 'pistolwhip-12')
            self.assertIn('2048-pixel eye render cap', info['reason'])
            request = dict(self.request, profile=info['profile'])
            manifest = u.validate_request(request, info, self.root / 'fresh-install')
        self.assertEqual(manifest['runtime_defaults'], {'KL_OVRP_EYE_MAX': '2048'})
        self.assertIn('setenv("KL_OVRP_EYE_MAX", "2048", 0)',
                      u.targets.build_target_source(self.resolve(manifest)))

    def test_pistolwhip_runtime_uses_its_own_app_and_the_audited_sdk(self):
        pistol = json.loads(u.STEAM_PROFILES['pistolwhip-12'].read_text())
        eleven = json.loads(u.STEAM_PROFILES['eleven'].read_text())
        self.assertEqual(pistol['app_id'], 1079800)
        self.assertEqual(pistol['interfaces'], eleven['interfaces'])
        for key in ('size', 'sha256'):
            self.assertEqual(pistol['files'][0][key], eleven['files'][0][key])
        self.assertEqual(u.audited_steam_profiles(self.info), [])
        sdk = self.tree/'lib/arm64-v8a/libsteam_api.so'
        sdk.write_bytes(bytes(pistol['files'][0]['size']))
        real_digest = u.digest
        with patch.object(u, 'digest', side_effect=lambda path: pistol['files'][0]['sha256'] if Path(path) == sdk else real_digest(path)):
            self.assertEqual(u.audited_steam_profiles(self.info), ['pistolwhip-12'])
            manifest = u.validate_request(self.request, self.info, self.root/'steam-build')
        self.assertEqual(manifest['steam_mode'], 'runtime')
        self.assertEqual(manifest['steam_profile'], 'pistolwhip-12')
        self.assertTrue(self.resolve(manifest)['steam_local'])

    def test_cached_experimental_request_gets_defaults_and_keeps_existing_identity(self):
        old = u.validate_request(self.request, self.info, self.root / 'cached-install')
        selected = u.validate_request(dict(self.request, profile='pistolwhip-12'),
                                      self.info, self.root / 'profile-install')
        expected = 'import-' + hashlib.sha256(self.info['package'].encode()).hexdigest()[:16]
        self.assertEqual(old['target'], expected)
        self.assertEqual(selected['target'], old['target'])
        self.assertEqual(selected['bundle'], old['bundle'])
        self.assertEqual(selected['runtime_profile'], 'pistolwhip-12')
        self.assertEqual(old['runtime_defaults'], {'KL_OVRP_EYE_MAX': '2048'})
        self.assertEqual(self.resolve(selected)['tree'], old['target'])

    def test_other_games_and_unverified_versions_do_not_get_pistolwhip_defaults(self):
        for changes in ({'package': 'test.other', 'apk_sha256': '0' * 64},
                        {'version_code': '13', 'apk_sha256': '0' * 64},
                        {'apk_sha256': '0' * 64}):
            with self.subTest(changes=changes):
                info = dict(self.info, **changes)
                self.assertEqual(u.match_profiles(info, self.tree), [])
                manifest = u.validate_request(self.request, info, self.root / 'unverified')
                self.assertEqual(manifest['runtime_defaults'], {})
                self.assertNotIn('setenv(', u.targets.build_target_source(self.resolve(manifest)))
        manifest = u.validate_request(dict(self.request, profile='pistolwhip-12'),
                                      dict(self.info, apk_sha256='0' * 64), self.root / 'manual')
        self.assertEqual(self.resolve(manifest)['runtime_defaults'], {})

    def test_compiled_defaults_work_without_saved_settings_and_allow_launch_override(self):
        if not shutil.which('clang'):
            self.skipTest('C compiler unavailable')
        manifest = u.validate_request(self.request, self.info, self.root / 'compiled')
        generated = self.root / 'BuildTarget.c'
        generated.write_text(u.targets.build_target_source(self.resolve(manifest)))
        harness = self.root / 'fresh.c'
        harness.write_text('''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "kl_target.h"
void kl_target_register_build(const kl_target *target) { assert(target); }
extern void kl_install_build_target(void);
int main(void) {
    unsetenv("KL_OVRP_EYE_MAX");
    kl_install_build_target();
    assert(!strcmp(getenv("KL_OVRP_EYE_MAX"), "2048"));
    setenv("KL_OVRP_EYE_MAX", "1536", 1);
    kl_install_build_target();
    assert(!strcmp(getenv("KL_OVRP_EYE_MAX"), "1536"));
    return 0;
}
''')
        executable = self.root / 'fresh'
        subprocess.run(['clang', '-Iruntime', str(harness), str(generated), '-o', str(executable)],
                       cwd=u.ROOT, capture_output=True, check=True)
        subprocess.run([str(executable)], check=True)

    def test_named_command_line_profile_has_the_same_defaults(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(u.targets, 'libs_for', return_value='libmain libunity'):
            target = u.targets.resolve('pistolwhip-12')
        self.assertEqual(target['runtime_defaults'], {'KL_OVRP_EYE_MAX': '2048'})
        self.assertTrue(target['bundle'].endswith('import-93242634a1ab5a06'))
        self.assertIn('setenv("KL_OVRP_EYE_MAX", "2048", 0)', u.targets.build_target_source(target))

    def test_project_embeds_defaults_with_and_without_an_installer_manifest(self):
        key = 'profile-test-' + uuid.uuid4().hex[:12]
        product = 'ProfileTest' + uuid.uuid4().hex[:12]
        target = dict(u.targets.TARGETS['pistolwhip-12'], name=key, product=product,
                      bundle='test.pistolwhip.profile', host_tree=str(self.tree), libs='libmain')
        self.addCleanup(lambda: shutil.rmtree(u.ROOT / 'visionos' / (product + '.xcodeproj'), ignore_errors=True))
        self.addCleanup(lambda: shutil.rmtree(u.ROOT / 'visionos/build/appicon' / (key + '.xcassets'), ignore_errors=True))
        for name in (f'{product}-BuildTarget.c', f'{product}-filtered.entitlements'):
            path = u.ROOT / 'visionos/build' / name
            self.addCleanup(lambda path=path: path.unlink(missing_ok=True))
        path = self.root / 'manifest.json'
        path.write_text('{}')
        for manifest_path in ('', str(path)):
            with (self.subTest(manifest=bool(manifest_path)),
                  patch.object(u.targets, 'resolve', return_value=target),
                  patch.dict(os.environ, {'KLEPTON_BUILD_MANIFEST': manifest_path,
                                          'KLEPTON_TEAM': 'TESTTEAM00', 'KLEPTON_STEAM_LOCAL': '0',
                                          'KL_CUSTOM_LAUNCHER': '0'}),
                  contextlib.redirect_stdout(io.StringIO())):
                project = runpy.run_path(str(u.ROOT / 'visionos/gen_xcodeproj.py'))
                self.assertTrue(project['BUILD_TARGET'])
                self.assertIn('KL_BUILD_MANIFEST=1', project['PBX'])
                self.assertIn('setenv("KL_OVRP_EYE_MAX", "2048", 0)', project['generated'].read_text())

    def test_invalid_runtime_defaults_are_rejected(self):
        manifest = u.validate_request(self.request, self.info, self.root / 'invalid')
        for defaults in ([], {'PATH': 'value'}, {'KL_TEST': 2048}, {'KL_TEST': 'a\0b'}):
            with self.subTest(defaults=defaults), self.assertRaisesRegex(ValueError, 'runtime defaults'):
                self.resolve(dict(manifest, runtime_defaults=defaults))


if __name__ == '__main__':
    unittest.main()
