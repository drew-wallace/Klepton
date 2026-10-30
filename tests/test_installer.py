"""Installer behavior with synthetic APKs and fake external command runners."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import utility as u


def elf():
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', data, 18, 183)
    return bytes(data)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.store = self.root/'store'
        self.store.mkdir()
        self.patch_store = patch.object(u, 'STORE', self.store)
        self.patch_store.start()
        self.addCleanup(self.patch_store.stop)
        self.addCleanup(self.temporary.cleanup)
        self.tree = self.root/'tree'
        (self.tree/'lib/arm64-v8a').mkdir(parents=True)
        for name in ('libmain', 'libunity'):
            (self.tree/'lib/arm64-v8a'/(name+'.so')).write_bytes(elf())
        (self.tree/'AndroidManifest.xml').write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="test.example"><application android:label="Example"/></manifest>')
        (self.tree/'apktool.yml').write_text("versionInfo:\n  versionCode: '42'\n  versionName: example\n")
        self.apk = self.root/'game `literal` $(literal) with spaces.apk'
        with zipfile.ZipFile(self.apk, 'w') as z:
            z.writestr('AndroidManifest.xml', (self.tree/'AndroidManifest.xml').read_bytes())
            z.writestr('lib/arm64-v8a/libmain.so', elf())
            z.writestr('lib/arm64-v8a/libunity.so', elf())
        self.info = dict(u.decoded_metadata(self.tree, ['libmain', 'libunity']),
                         host_tree=str(self.tree), apk_path=str(self.apk), import_id=u.digest(self.apk))
        self.request = dict(schema_version=1, import_id=self.info['import_id'], profile='',
                            steam_mode='auto', kind='unity', entry='libmain', team='ABCDEFGHIJ', device='device', data=[])

    def test_diagnostics_classify_real_failures_without_treating_success_as_jni_failure(self):
        self.assertEqual(u.diagnose_logs("[klepton] fatal: called unresolved import 'putenv'")['category'], 'runtime')
        self.assertIn('putenv', u.diagnose_logs("fatal: called unresolved import 'putenv'")['message'])
        self.assertEqual(u.diagnose_logs('[jni] UNIMPLEMENTED JNIEnv slot 5: example')['category'], 'runtime')
        unsupported = u.diagnose_logs('[jni] no host implementation for com/vuplex/webview/WebView3D.setAlternativePointerInputSystemEnabled(Z)V')
        self.assertEqual(unsupported['category'], 'runtime')
        self.assertIn('WebView3D.setAlternativePointerInputSystemEnabled', unsupported['message'])
        self.assertEqual(u.diagnose_logs('initJni completed, no unimplemented JNI calls')['category'], 'unknown')
        self.assertEqual(u.diagnose_logs('Failed to read data for the AssetBundle')['category'], 'data')
        self.assertEqual(u.diagnose_logs('[klepton] fatal: runtime fault')['category'], 'crash')
        self.assertEqual(u.diagnose_logs('eglSwapBuffers: 42')['category'], 'unknown')
        browser = u.diagnose_logs('E/Unity: WebViewUnavailableException: browser unavailable\n[klepton] fault: signal 11')
        self.assertEqual(browser['category'], 'runtime')
        self.assertIn('JavaScript bridge', browser['message'])

    def test_webkit_failures_have_actionable_diagnostics(self):
        failure = u.diagnose_logs('[webview] navigation failed -1009: offline')
        self.assertEqual(failure['category'], 'network')
        self.assertIn('connectivity', failure['message'])
        failure = u.diagnose_logs('[webview] snapshot failed: internal error')
        self.assertEqual(failure['category'], 'runtime')
        self.assertIn('page texture', failure['message'])

    def test_missing_meta_account_is_distinguished_from_network_failure(self):
        for log in ('[plat] account service unavailable: ovr_User_GetLoggedInUser',
                    'Meta account services are unavailable on this host.'):
            failure = u.diagnose_logs(log)
            self.assertEqual(failure['category'], 'account')
            self.assertIn('authenticated Meta account provider', failure['message'])
            self.assertIn('email/password', failure['message'])
            self.assertIn('not evidence of an internet outage', failure['message'])
        # The account warning must not hide an independent unsupported call.
        self.assertEqual(u.diagnose_logs('[plat] account service unavailable:\n'
                         "fatal: called unresolved import 'putenv'")['category'], 'runtime')
        self.assertEqual(u.diagnose_logs('[plat] account service unavailable:\n'
                         '[klepton] fault: signal 11')['category'], 'crash')

    def test_login_input_failure_is_not_hidden_by_startup_meta_warning(self):
        log = ('[plat] account service unavailable: ovr_User_GetLoggedInUser\n'
               'E/Unity: <color=red> HTTP error: Input error. Call GetErrorList() to get the full list.</color>')
        failure = u.diagnose_logs(log)
        self.assertEqual(failure['category'], 'account')
        self.assertIn('input-validation', failure['message'])
        self.assertIn('fields accepted your text', failure['message'])
        self.assertIn('does not identify the rejected field', failure['message'])
        self.assertNotIn('try that route', failure['message'])
        self.assertEqual(u.diagnose_logs(log+'\n[klepton] fault: signal 11')['category'], 'crash')

    def test_known_bigscreen_browser_warning_is_limited_to_the_tested_apk(self):
        fingerprint = '6cae96955397be3522c238196635a061c5f9da465f032cbe34b817dddbb4a18a'
        self.assertIn('Known Bigscreen limitation', u.inspection_warning({'apk_sha256': fingerprint}))
        self.assertNotIn('Known Bigscreen limitation', u.inspection_warning({'apk_sha256': 'other'}))

    def test_gles_compatibility_is_validated_and_baked_only_when_selected(self):
        manifest = u.validate_request(self.request, self.info, self.root/'job')
        self.assertEqual(manifest['gles_version'], '')
        with self.assertRaises(u.UtilityError):
            u.validate_request(dict(self.request, gles_version='3.2'), self.info, self.root/'job')
        with self.assertRaises(u.UtilityError):
            u.validate_request(dict(self.request, gles_version='3.0', kind='native'), self.info, self.root/'job')
        manifest = u.validate_request(dict(self.request, gles_version='3.0'), self.info, self.root/'job')
        path = self.root/'manifest.json'
        u.write_json(path, manifest)
        target = u.targets.resolve_manifest(path)
        self.assertIn('kl_egl_set_gles_version(3, 0)', u.targets.build_target_source(target))
        manifest = u.validate_request(dict(self.request, gles_version='3.1'), self.info, self.root/'job')
        u.write_json(path, manifest)
        self.assertIn('kl_egl_set_gles_version(3, 1)', u.targets.build_target_source(u.targets.resolve_manifest(path)))
        target['gles_version'] = ''
        self.assertNotIn('kl_egl_set_gles_version', u.targets.build_target_source(target))

    def test_diagnostics_only_copy_logs_and_keep_previous_failure_out_of_current_diagnosis(self):
        job = self.root/'install'
        job.mkdir()
        u.write_json(job/'manifest.json', dict(bundle='dev.test.example', device='headset', display='Example',
                                               team='private-team', data=['private-save-folder'], gles_version='3.0'))
        runner = u.Runner(self.root/'diagnose')
        copied = []
        def copy(argv, **kwargs):
            source = argv[argv.index('--source')+1]
            destination = Path(argv[argv.index('--destination')+1])
            copied.append(source)
            if source.endswith('klepton-crash.log'):
                raise u.UtilityError('No crash file')
            content = "fatal: called unresolved import 'old_failure'" if source.endswith('.previous') else 'initJni completed, no unimplemented JNI calls'
            destination.write_text(content)
        with patch.object(u.Device, 'app', return_value={'bundleIdentifier':'dev.test.example'}), patch.object(runner, 'run', side_effect=copy):
            result = u.collect_diagnostics(job, runner)
        self.assertEqual(result['category'], 'unknown')
        self.assertEqual(len(copied), 4)
        self.assertTrue(all(p.startswith('Documents/klepton-') for p in copied))
        report = json.loads((Path(result['diagnostics_path'])/'report.json').read_text())
        self.assertEqual(report['unavailable'], ['klepton-crash.log'])
        self.assertEqual(report['app']['gles_version'], '3.0')
        self.assertNotIn('private-team', json.dumps(report))
        self.assertNotIn('private-save-folder', json.dumps(report))

    def test_diagnostics_cancellation_is_not_swallowed_as_a_missing_log(self):
        job = self.root/'install'
        job.mkdir()
        u.write_json(job/'manifest.json', dict(bundle='dev.test.example', device='headset'))
        runner = u.Runner(self.root/'diagnose')
        def cancel(*args, **kwargs):
            runner.cancelled = True
            raise u.Cancelled('cancelled')
        with patch.object(u.Device, 'app', return_value={}), patch.object(runner, 'run', side_effect=cancel):
            with self.assertRaises(u.Cancelled):
                u.collect_diagnostics(job, runner)

    def test_native_detection_and_quoted_filename(self):
        libraries, _ = u.archive_inventory(self.apk)
        self.assertEqual(libraries, ['libmain', 'libunity'])
        info = u.decoded_metadata(self.tree, libraries)
        self.assertEqual((info['kind'], info['entry'], info['version_code']), ('unity', 'libmain', '42'))

    def test_signing_team_uses_certificate_ou_not_personal_display_identifier(self):
        fingerprint='AB'*20
        def captured(args, timeout=15, input_text=None):
            if 'find-identity' in args:
                return f'  1) {fingerprint} "Apple Development: Example Person (PERSON1234)"\n     1 valid identities found\n'
            if 'find-certificate' in args:
                return '-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----'
            if args[0]=='openssl':
                self.assertIn('fixture',input_text)
                return 'subject=C=US,O=Example,OU=TEAM123456,CN=Apple Development: Example Person (PERSON1234)\nsha1 Fingerprint='+':'.join(['AB']*20)+'\n'
            return ''
        with patch.object(u,'capture',side_effect=captured):
            result=u.signing_teams()
        self.assertEqual([t['id'] for t in result],['TEAM123456'])
        self.assertIn('Team TEAM123456',result[0]['name'])

    def test_signing_certificates_without_matching_private_identity_are_not_offered(self):
        def captured(args, timeout=15, input_text=None):
            if 'find-identity' in args:return '0 valid identities found'
            if 'find-certificate' in args:return '-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----'
            return 'subject=OU=TEAM123456,CN=Apple Development: Example\nsha1 Fingerprint='+':'.join(['AB']*20)+'\n'
        with patch.object(u,'capture',side_effect=captured):
            self.assertEqual(u.signing_teams(),[])

    def test_import_snapshots_without_changing_original_and_reuses_decode(self):
        original = self.apk.read_bytes()
        fixture = self.tree
        class Decoder(FakeRunner):
            def run(self, args, env=None, timeout=None):
                self.commands.append(args)
                import shutil
                shutil.copytree(fixture, Path(args[args.index('-o')+1]))
        runner=Decoder(self.root/'decode')
        with patch.object(u.shutil,'which',return_value='fixture-apktool'), patch.object(u.targets,'libs_for',return_value='libmain libunity'), patch.object(u,'profile_catalog',return_value={}):
            first=u.inspect_apk(self.apk,runner)
            second=u.inspect_apk(self.apk,runner)
        self.assertEqual(self.apk.read_bytes(),original)
        self.assertEqual(Path(first['apk_path']).read_bytes(),original)
        self.assertNotEqual(Path(first['host_tree']),fixture)
        self.assertEqual(first['import_id'],second['import_id'])
        self.assertEqual(len(runner.commands),1)
        # A subsequent build can recreate a pruned decode entirely locally.
        import shutil
        shutil.rmtree(first['host_tree'])
        with patch.object(u.shutil,'which',return_value='fixture-apktool'):
            u.decode_import(Path(first['apk_path']),Path(first['host_tree']),runner)
        self.assertTrue((Path(first['host_tree'])/'AndroidManifest.xml').is_file())
        self.assertEqual(len(runner.commands),2)

    def test_unsafe_split_and_wrong_architecture(self):
        for filename, content, expected in [('../escape', b'', 'unsafe'), ('base.apk', b'', 'split'),
                ('lib/arm64-v8a/libmain.so', elf()[:18]+b'\x03\x00'+elf()[20:], 'ARM64')]:
            with self.subTest(filename=filename), zipfile.ZipFile(self.root/'bad.apk', 'w') as z:
                z.writestr('AndroidManifest.xml', b'<manifest/>')
                z.writestr(filename, content)
            with self.assertRaisesRegex(u.UtilityError, expected):
                u.archive_inventory(self.root/'bad.apk')
        (self.root/'bad.apk').write_text('not zip')
        with self.assertRaisesRegex(u.UtilityError, 'Invalid APK'):
            u.archive_inventory(self.root/'bad.apk')

    def test_absent_native_and_split_manifest(self):
        with zipfile.ZipFile(self.root/'java.apk', 'w') as z:
            z.writestr('AndroidManifest.xml', b'<manifest/>')
        with self.assertRaisesRegex(u.UtilityError, 'JVM'):
            u.archive_inventory(self.root/'java.apk')
        (self.tree/'AndroidManifest.xml').write_text('<manifest package="test.example" split="config.arm64"/>')
        with self.assertRaisesRegex(u.UtilityError, 'Split'):
            u.decoded_metadata(self.tree, ['libmain'])

    def test_unknown_and_ambiguous_detection(self):
        (self.tree/'AndroidManifest.xml').write_text('<manifest package="test.example"/>')
        self.assertEqual(u.decoded_metadata(self.tree, ['libsomething'])['kind'], '')
        self.assertEqual(u.decoded_metadata(self.tree, ['libmain', 'libunity', 'libUE4'])['kind'], '')
        p = dict(package='test.example', version_code='42', entry_sha256=u.digest(self.tree/'lib/arm64-v8a/libmain.so'))
        with patch.dict(u.targets.TARGETS, {'fixture-a': {'entry':'libmain'}, 'fixture-b': {'entry':'libmain'}}):
            self.assertEqual(u.match_profiles(self.info, self.tree, {'fixture-a':p,'fixture-b':p}), ['fixture-a','fixture-b'])

    def test_manifest_keeps_canonical_identity_and_isolates_inputs(self):
        self.request.update(profile='eleven')
        m = u.validate_request(self.request, self.info, self.root/'abc123')
        path = self.root/'manifest.json'
        path.write_text(json.dumps(m))
        with patch.object(u.targets, 'libs_for', return_value='libmain libunity'):
            t = u.targets.resolve_manifest(path, 'eleven')
        self.assertEqual(t['name'], 'eleven')
        self.assertEqual(t['tree'], 'eleven')
        self.assertEqual(t['host_tree'], str(self.tree.resolve()))
        self.assertNotEqual(t['framework_key'], 'eleven')
        self.assertEqual(u.targets.TARGETS['eleven']['tree'], 'eleven')
        with patch.dict(os.environ, {'KLEPTON_BUILD_MANIFEST':str(path)}), patch.object(u.targets, 'libs_for', return_value='libmain'):
            self.assertEqual(u.targets.resolve('eleven')['host_apk'], str(self.apk.resolve()))

    def test_unknown_manifest_and_invalid_entry(self):
        m = u.validate_request(self.request, self.info, self.root/'abc123')
        path = self.root/'manifest.json'
        path.write_text(json.dumps(m))
        with patch.object(u.targets, 'libs_for', return_value='libmain'):
            resolved = u.targets.resolve_manifest(path)
            self.assertEqual(resolved['kind'], 'unity')
            source = u.targets.build_target_source(resolved)
            self.assertIn('kl_target_register_build', source)
            self.assertIn('KL_GUEST_UNITY', source)
            self.assertNotIn(str(self.tree), source)
            self.assertIn(m['target']+'.apk', source)
        self.request['entry'] = 'libmissing'
        with self.assertRaisesRegex(u.UtilityError, 'entry library'):
            u.validate_request(self.request, self.info, self.root/'abc123')

    def test_steam_audit_and_auto_offline(self):
        self.info['steam'] = True
        with patch.object(u, 'steam_audit', return_value=False):
            self.assertEqual(u.resolve_steam('auto', self.info, None), 'offline')
            with self.assertRaisesRegex(u.UtilityError, 'audited'):
                u.resolve_steam('runtime', self.info, None)
        with patch.object(u, 'steam_audit', return_value=True):
            self.assertEqual(u.resolve_steam('auto', self.info, 'eleven'), 'runtime')
        lock = self.root/'steam.json'
        binary = self.tree/'lib/arm64-v8a/libmain.so'
        lock.write_text(json.dumps({'files':[{'path':'old/libmain.so', 'size':64, 'sha256':u.digest(binary)}]}))
        with patch.dict(u.STEAM_PROFILES, {'fixture':lock}):
            self.assertTrue(u.steam_audit('fixture', self.tree))
            binary.write_bytes(b'different')
            self.assertFalse(u.steam_audit('fixture', self.tree))

    def test_steam_channel_is_validated_and_forwarded_to_every_build_stage(self):
        self.request['steam_channel'] = 'beta'
        m = u.validate_request(self.request, self.info, self.root/'abc123')
        self.assertEqual(m['steam_channel'], 'beta')
        with patch.dict(os.environ, {'KLEPTON_STEAM_CHANNEL': 'stable'}):
            self.assertEqual(u.environment(self.root/'manifest.json', m)['KLEPTON_STEAM_CHANNEL'], 'beta')
        self.request['steam_channel'] = 'preview'
        with self.assertRaisesRegex(ValueError, 'channel'):
            u.validate_request(self.request, self.info, self.root/'abc123')

    def test_obb_validation_and_retail_destination(self):
        self.request.update(profile='roborecall-41778', kind='ue4', entry='libmain')
        with self.assertRaisesRegex(u.UtilityError, 'external game data'):
            u.validate_request(self.request, self.info, self.root/'job')
        wrong = self.root/'main.41.test.example.obb';wrong.write_bytes(b'data')
        self.request['data'] = [str(wrong)]
        with self.assertRaisesRegex(u.UtilityError, 'main.42.test.example.obb'):
            u.validate_request(self.request, self.info, self.root/'job')
        correct = self.root/'main.42.test.example.obb';correct.write_bytes(b'data')
        self.request['data'] = [str(correct)]
        self.assertEqual(u.validate_request(self.request, self.info, self.root/'job')['obb_path'], 'Android/obb/com.YourCompany.RoboRecall')

    def test_save_conflicts_and_duplicate_data(self):
        folder = self.root/'data';folder.mkdir();(folder/'save.dat').write_bytes(b'seed');(folder/'asset.bundle').write_bytes(b'asset')
        m = u.validate_request(dict(self.request, data=[str(folder)], data_path='files'), self.info, self.root/'job')
        payload = u.prepare_payload(m, self.root/'job', [{'path':'Documents/android-files/files/save.dat'}])
        self.assertEqual(list(payload), ['Documents/android-files/files/asset.bundle'])
        m['data'] += [str(folder)]
        with self.assertRaisesRegex(u.UtilityError, 'duplicate'):
            u.prepare_payload(m, self.root/'job2', [])

    def test_steam_data_destination_is_independent_of_walkabout_compatibility_version(self):
        folder = self.root/'download'/'obb'
        folder.mkdir(parents=True)
        (folder/'course.bundle').write_bytes(b'course')
        (folder/'main.59349.com.MightyCoconut.WalkaboutMiniGolf.obb').write_bytes(b'obb')
        info = dict(self.info, package='com.MightyCoconut.WalkaboutMiniGolf', version_code='59349',
                    version_name='6.8', suggested_data=[str(folder)], suggested_data_path='obb')
        request = dict(self.request, data=[str(folder)], data_path='')
        manifest = u.validate_request(request, info, self.root/'job')
        self.assertIsNone(manifest['profile'])
        self.assertEqual(manifest['data_path'], 'obb')
        self.assertEqual(manifest['obb_path'], 'obb')
        payload = u.prepare_payload(manifest, self.root/'job', [])
        self.assertEqual(set(payload), {'Documents/android-files/obb/course.bundle',
            'Documents/android-files/obb/main.59349.com.MightyCoconut.WalkaboutMiniGolf.obb'})

    def test_steam_destination_hint_does_not_guess_for_other_loose_folders(self):
        downloaded = self.root/'download'/'obb'; downloaded.mkdir(parents=True)
        other = self.root/'manual'; other.mkdir(); (other/'asset.bundle').write_bytes(b'asset')
        info = dict(self.info, suggested_data=[str(downloaded)], suggested_data_path='obb')
        for selected in ([str(other)], [str(downloaded), str(other)]):
            with self.subTest(selected=selected), self.assertRaisesRegex(u.UtilityError, 'inside the game'):
                u.validate_request(dict(self.request, data=selected), info, self.root/'job')
        manifest = u.validate_request(dict(self.request, data=[str(downloaded)], data_path='custom'), info, self.root/'job')
        self.assertEqual(manifest['data_path'], 'custom')

    def test_unmapped_experimental_folder_requires_a_relative_destination(self):
        folder = self.root/'manual'; folder.mkdir(); (folder/'asset.bundle').write_bytes(b'asset')
        with self.assertRaisesRegex(u.UtilityError, 'not enter a folder path on your Mac'):
            u.validate_request(dict(self.request, data=[str(folder)]), self.info, self.root/'job')
        with self.assertRaisesRegex(u.UtilityError, 'safe relative'):
            u.validate_request(dict(self.request, data=[str(folder)], data_path='../outside'), self.info, self.root/'job')

    def test_backup_groups_never_include_immutable_obb(self):
        files = ['Documents/guest.apk', 'Documents/android-files/obb/main.obb',
                 'Documents/android-files/Game/saved/a.sav', 'Documents/android-files/Game/saved/b.sav',
                 'Library/Preferences/test.plist']
        wanted = files[2:]
        self.assertEqual(u.backup_groups(files, wanted), ['Documents/android-files/Game', 'Library'])

    def test_file_inventory_fails_closed(self):
        runner = FakeRunner(self.root/'job')
        device = u.Device(runner, 'device', 'test.bundle')
        runner.responses = [{'files':[{'relativePath':'Documents/save', 'resources':{'isDirectory':False}}]}]
        self.assertEqual(device.files(), ['Documents/save'])
        runner.responses = [{'files':[{'relativePath':'Documents/save'}]}]
        with self.assertRaisesRegex(u.UtilityError, 'unknown file type'):
            device.files()

    def test_backup_failure_prevents_install(self):
        runner = FakeRunner(self.root/'job')
        runner.fail = 'copy'
        device = u.Device(runner, 'device', 'test.bundle')
        runner.responses = [{'apps':[{'bundleIdentifier':'test.bundle', 'url':'file:///App.app'}]}, {'runningProcesses':[]},
                            {'files':[{'relativePath':'Documents/save', 'resources':{'isDirectory':False}}]}]
        with self.assertRaises(u.UtilityError), contextlib.redirect_stdout(io.StringIO()):
            device.backup(self.root/'job', {'target':'fixture'})
        self.assertFalse(any('install' in call for call in runner.commands))

    def test_runner_preserves_argument_boundaries_and_failure(self):
        runner = u.Runner(self.root/'job')
        runner.run([sys.executable, '-c', 'import sys;print(sys.argv[1])', str(self.apk)])
        self.assertIn(str(self.apk), runner.log_path.read_text())
        with self.assertRaisesRegex(u.UtilityError, 'exit 7'):
            runner.run([sys.executable, '-c', 'import sys;sys.exit(7)'])

    def test_checkout_lock_and_signal_cancel(self):
        with u.checkout_lock(), self.assertRaisesRegex(u.UtilityError, 'Another'):
            with u.checkout_lock():
                pass
        runner = u.Runner(self.root/'cancel')
        runner.cancelled = True
        with self.assertRaises(u.Cancelled):
            runner.run([sys.executable, '-c', 'raise Exception("should never run")'])

    def test_real_subprocess_cancellation_stops_descendants(self):
        script = self.root/'cancel.py'
        script.write_text('import sys\nsys.path.insert(0,'+repr(str(u.ROOT/'tools'))+')\nimport utility as u\nr=u.Runner('+repr(str(self.root/'cancel'))+')\nr.install_handlers()\ntry:r.run([sys.executable,"-c","import time;time.sleep(100)"])\nexcept u.Cancelled:sys.exit(130)\n')
        child = subprocess.Popen([sys.executable, str(script)])
        try:
            time.sleep(.3)
            child.terminate()
            self.assertEqual(child.wait(timeout=5), 130)
        finally:
            if child.poll() is None: child.kill();child.wait()


    def test_runtime_target_registration_and_fallback(self):
        if sys.platform != 'darwin':
            self.skipTest('native compiler gate runs on macOS')
        harness = self.root/'target.c'
        harness.write_text(r'''#include <assert.h>
#include <string.h>
#include "kl_target.h"
const char *kl_env_str(const char *n,const char *d){return d;}
const char *kl_userdata_dir(const char *n){return n;}
void kl_set_library_path(const char *p){}
void kl_jni_set_native_lib_dir(const char *p){}
void kl_jni_set_assets_dir(const char *p){}
void kl_jni_set_apk_path(const char *p){}
void kl_jni_set_files_dir(const char *p){}
void kl_jni_set_obb_rel(const char *p){}
int main(void){
 static const kl_target imported = {.name="unknown-import",.entry_lib="libcustom",.kind=KL_GUEST_NATIVE};
 static const kl_target known = {.name="eleven",.tree="imported-tree",.kind=KL_GUEST_UNITY};
 const kl_target *original = kl_target_lookup("eleven"); assert(original);
 kl_target_register_build(&imported);
 assert(kl_target_lookup("unknown-import")==&imported);
 assert(kl_target_default()==&imported);
 assert(kl_target_lookup("eleven")==original);
 kl_target_register_build(&known);
 assert(kl_target_lookup("eleven")==&known);
 assert(!strcmp(kl_target_default()->tree,"imported-tree"));
 kl_target_register_build(0); assert(kl_target_lookup("eleven")==original);
 return 0;
}
''')
        executable = self.root/'target'
        manifest = u.validate_request(self.request, self.info, self.root/'native-registry')
        path = self.root/'native-manifest.json'
        path.write_text(json.dumps(manifest))
        with patch.object(u.targets, 'libs_for', return_value='libmain libunity'):
            generated = self.root/'BuildTarget.c'
            generated.write_text(u.targets.build_target_source(u.targets.resolve_manifest(path)))
        source = harness.read_text().replace('int main(void){', 'extern void kl_install_build_target(void);\nint main(void){')
        source = source.replace(' return 0;', ' kl_install_build_target(); assert(!strcmp(kl_target_default()->name, '+json.dumps(manifest['target'])+'));\n return 0;')
        harness.write_text(source)
        subprocess.run(['clang', '-Iruntime', '-Iruntime/jni', '-Iruntime/libc', '-Iruntime/gfx', '-Iruntime/xr',
                        str(harness), str(generated), 'runtime/kl_target.c', '-o', str(executable)], cwd=u.ROOT, check=True, capture_output=True)
        subprocess.run([str(executable)], check=True)

    def pipeline(self, fail='', missing_app=False, saved=False, container='container-one'):
        jobs = self.store/'jobs'
        runner = PipelineRunner(jobs/('job-'+str(len(list(jobs.glob('job-*'))))), fail, missing_app)
        self.last_runner = runner
        owner = self
        class FakeDevice:
            def __init__(self, runner, identifier, bundle): self.runner=runner;self.bundle=bundle
            def app(self): return {'dataContainerPath':container}
            def backup(self, job, m):
                if fail == 'backup': raise u.UtilityError('backup failed')
                if not saved: return []
                p=job/'backup/Documents/save';p.parent.mkdir(parents=True);p.write_bytes(b'saved')
                records=[dict(path='Documents/save',sha256=u.digest(p))]
                u.write_json(job/'backup.json',dict(files=records))
                return records
            def restore(self, job, records):
                if fail == 'restore': raise u.UtilityError('restore failed')
            def copy(self, direction, source, destination):
                self.runner.commands.append(['copy',direction,str(source),str(destination)])
                if direction == 'from': Path(destination).write_text(json.dumps(owner.remote_receipt))
                elif destination == 'Documents/klepton-installer-transfer.json': owner.remote_receipt=json.loads(Path(source).read_text())
        readiness=dict(checks=[dict(name='tools',ready=True)], teams=[dict(id='ABCDEFGHIJ')], devices=[dict(id='device',paired=True)])
        with patch.object(u,'load_inspection',return_value=self.info), patch.object(u,'doctor',return_value=readiness), \
                patch.object(u,'Device',FakeDevice), contextlib.redirect_stdout(io.StringIO()):
            result=u.run_job(self.request,runner)
        return runner,result

    def test_pipeline_success_records_container_and_never_launches(self):
        self.remote_receipt={}
        runner,result=self.pipeline()
        self.assertEqual(result['app_path'], '')
        self.assertFalse((runner.directory/'DerivedData').exists())
        self.assertTrue((runner.directory/'manifest.json').is_file())
        self.assertTrue((runner.directory/'result.json').is_file())
        self.assertFalse(any('launch' in c for c in runner.commands))
        receipts=list((self.store/'receipts').iterdir())
        self.assertEqual(len(receipts),1)
        self.assertEqual(json.loads(receipts[0].read_text())['container'],'container-one')

    def test_completed_install_discards_working_files_but_keeps_downloads_and_evidence(self):
        self.remote_receipt={}
        imported=self.store/'imports'/self.info['import_id']
        (imported/'tree').mkdir(parents=True)
        (imported/'tree/AndroidManifest.xml').write_text('decoded')
        (imported/'source.apk').write_bytes(b'original APK')
        download=self.store/'steam-library/downloads/course.bundle'
        download.parent.mkdir(parents=True)
        download.write_bytes(b'downloaded course')
        runner,result=self.pipeline(saved=True)
        self.assertFalse((runner.directory/'backup').exists())
        self.assertFalse((runner.directory/'backup.json').exists())
        self.assertFalse((imported/'tree').exists())
        self.assertEqual((imported/'source.apk').read_bytes(),b'original APK')
        self.assertEqual(download.read_bytes(),b'downloaded course')
        self.assertTrue((runner.directory/'transfer.json').is_file())

    def test_cleanup_leaves_pending_recovery_and_unfinished_jobs_untouched(self):
        job=self.store/'jobs/recovery'
        (job/'backup').mkdir(parents=True)
        (job/'backup/save').write_bytes(b'only save copy')
        u.write_json(job/'manifest.json',{})
        self.assertEqual(u.cleanup_installed_job(job),[])
        u.write_json(job/'result.json',{})
        u.write_json(job/'transfer.json',{})
        u.write_json(self.store/'pending/fixture',dict(job=str(job)))
        self.assertEqual(u.cleanup_installed_job(job),[])
        self.assertEqual((job/'backup/save').read_bytes(),b'only save copy')

    def test_signing_failure_and_missing_product_never_install_stale_app(self):
        for failure,missing in [('xcodebuild',False),('',True)]:
            with self.subTest(failure=failure),self.assertRaises(u.UtilityError):
                self.pipeline(fail=failure,missing_app=missing)
            self.assertFalse(any('install' in c for c in self.last_runner.commands))
        self.assertFalse((self.store/'receipts').exists())

    def test_transfer_retry_and_receipt_container_invalidation(self):
        self.remote_receipt={}
        with self.assertRaises(u.UtilityError):self.pipeline(fail='stage_assets.sh')
        self.assertFalse(list((self.store/'receipts').glob('*')))
        self.pipeline()
        reused,_=self.pipeline()
        self.assertTrue(any('--meta' in c for c in reused.commands))
        changed,_=self.pipeline(container='container-two')
        self.assertFalse(any('--meta' in c for c in changed.commands))

    def test_restore_failure_retains_original_backup_and_blocks_retry(self):
        self.remote_receipt={}
        with self.assertRaisesRegex(u.UtilityError,'restore failed'):
            self.pipeline(fail='restore',saved=True)
        pending=list((self.store/'pending').iterdir())
        self.assertEqual(len(pending),1)
        original=json.loads(pending[0].read_text())['job']
        self.assertEqual((Path(original)/'backup/Documents/save').read_bytes(),b'saved')
        with self.assertRaisesRegex(u.UtilityError,'Restore'):
            self.pipeline()

    def test_explicit_recovery_keeps_current_snapshot_and_then_clears_pending(self):
        self.remote_receipt={}
        with self.assertRaises(u.UtilityError):
            self.pipeline(fail='restore',saved=True)
        pointer=next((self.store/'pending').iterdir())
        original=Path(json.loads(pointer.read_text())['job'])
        current=FakeRunner(self.store/'jobs'/'recovery')
        calls=[]
        class RecoveryDevice:
            def __init__(self,*_): pass
            def app(self): return {'url':'file:///Game.app'}
            def backup(self,job,manifest): calls.append(('snapshot',job))
            def restore(self,job,records): calls.append(('restore',job));self.records=records
        # Production jobs live under STORE/jobs; move the fixture accordingly.
        actual=self.store/'jobs'/'original'
        actual.parent.mkdir(exist_ok=True)
        original.rename(actual)
        u.write_json(pointer,dict(job=str(actual.resolve())))
        with patch.object(u,'Device',RecoveryDevice):
            result=u.recover_job(actual,current)
        self.assertEqual(calls,[('snapshot',current.directory),('restore',actual.resolve())])
        self.assertFalse(pointer.exists())
        self.assertEqual(result['job_path'],str(current.directory))

    def test_running_app_stops_backup(self):
        runner=FakeRunner(self.root/'running')
        runner.responses=[{'runningProcesses':[{'executable':'file:///Game.app/Game'}]}]
        with self.assertRaisesRegex(u.UtilityError,'Close the existing game'):
            u.Device(runner,'device','test.bundle').ensure_closed({'url':'file:///Game.app'})

    def test_changed_backup_cannot_overwrite_saved_data(self):
        runner=FakeRunner(self.root/'changed-backup')
        path=runner.directory/'backup/Documents/save'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'original')
        record=dict(path='Documents/save',sha256=u.digest(path))
        path.write_bytes(b'changed')
        with self.assertRaisesRegex(u.UtilityError,'backup file changed'):
            u.Device(runner,'device','test.bundle').restore(runner.directory,[record])
        self.assertFalse(runner.commands)

    def test_cancel_before_install_does_not_create_recovery_record(self):
        runner=PipelineRunner(self.root/'cancelled')
        class CancelBeforeInstall:
            def __init__(self,*_): pass
            def app(self):return {'dataContainerPath':'container'}
            def backup(self,*_):return []
        def cancel():raise u.Cancelled('cancelled')
        runner.check_cancelled=cancel
        ready=dict(checks=[],teams=[dict(id='ABCDEFGHIJ')],devices=[dict(id='device',paired=True)])
        with patch.object(u,'load_inspection',return_value=self.info),patch.object(u,'doctor',return_value=ready),patch.object(u,'Device',CancelBeforeInstall),contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(u.Cancelled):u.run_job(self.request,runner)
        self.assertFalse((self.store/'pending').exists())


class FakeRunner:
    def __init__(self, directory):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)
        self.log_path = directory/'build.log'
        self.commands = []
        self.responses = []
        self.stage = ''
        self.fail = ''
    def start(self, stage, message): self.stage = stage
    def check_cancelled(self): pass
    def run(self, args, env=None, timeout=None):
        self.commands.append([str(a) for a in args])
        if self.fail and self.fail in args: raise u.UtilityError('injected failure')
    def dev_json(self, args):
        self.commands.append(args)
        return self.responses.pop(0)



class PipelineRunner(FakeRunner):
    def __init__(self,directory,fail='',missing_app=False):
        super().__init__(directory)
        self.fail=fail;self.missing_app=missing_app;self.recovery_job=''
    def run(self,args,env=None,timeout=None):
        args=[str(a) for a in args];self.commands.append(args)
        if self.fail and any(self.fail in a for a in args):raise u.UtilityError('injected '+self.fail)
        if args[0]=='xcodebuild' and not self.missing_app:
            derived=Path(args[args.index('-derivedDataPath')+1]);product=Path(args[args.index('-project')+1]).stem
            app=derived/'Build/Products/Debug-xros'/(product+'.app');app.mkdir(parents=True)
            (app/'Info.plist').write_text('fixture')

if __name__ == '__main__':
    unittest.main()
