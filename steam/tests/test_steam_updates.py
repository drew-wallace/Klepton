"""Channel routing, signed update publication, and failure isolation."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]/'tools'
sys.path.insert(0, str(TOOLS))
import steam_updates as updates
import steam_auth_ui
import steam_runtime


def feed(channel='stable', version='0.4.5', build='20261022.6150929'):
    return {'minor': {'candidates': [dict(
        image=dict(arch='aarch64', branch=channel, product='steamos', release='holo', variant='vr', version=version, buildid=build),
        update_path=f'vr/{build}/deckard-{channel}-{build}-{version}.raucb', chunks_store_path='vr/chunks.castr')]}}


class SteamUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.store_patch = patch.object(updates, 'STORE', self.root/'updates')
        self.store_patch.start()
        self.addCleanup(self.store_patch.stop)

    def publish(self, channel, build, version, content=b'client'):
        generation = ('a' if channel == 'stable' else 'b') * 32
        root = updates.STORE/'builds'/build/generation
        path = root/'steam/androidarm64/libsteamclient.so'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        receipt = dict(build=build, version=version, compatible=True, files=[dict(
            path='androidarm64/libsteamclient.so', size=len(content), sha256=updates.digest(path))])
        updates.write_json(root/'receipt.json', receipt)
        updates.write_json(updates.STORE/'channels'/(channel+'.json'), dict(build=build, version=version, generation=generation))
        return path

    def test_valve_feeds_select_only_the_requested_arm64_vr_channel(self):
        stable = feed()
        stable['minor']['candidates'].insert(0, feed('beta')['minor']['candidates'][0])
        stable['minor']['candidates'].append(feed(build='20261025.1')['minor']['candidates'][0])
        selected = updates.candidate(stable, 'stable')
        self.assertEqual(selected['build'], '20261025.1')
        self.assertEqual(selected['channel'], 'stable')
        with self.assertRaisesRegex(ValueError, 'advertise'):
            updates.candidate(feed(), 'beta')
        for field, value in [('arch', 'x86_64'), ('variant', 'handheld'), ('product', 'other')]:
            document = feed()
            document['minor']['candidates'][0]['image'][field] = value
            with self.assertRaises(ValueError): updates.candidate(document, 'stable')

    def test_remote_metadata_cannot_escape_the_valve_store(self):
        for value in ['../escape', 'vr/../escape.raucb', 'https://other.test/image', 'vr/x?file=evil']:
            document = feed()
            document['minor']['candidates'][0]['update_path'] = value
            with self.assertRaises(ValueError): updates.candidate(document, 'stable')
        with self.assertRaises(ValueError): updates.channel_name('preview')

    def test_channels_resolve_to_independent_immutable_payloads(self):
        stable = self.publish('stable', '20261022.1', '0.4.5', b'stable')
        beta = self.publish('beta', '20261022.2', '0.5.6', b'beta')
        a, _ = updates.resolve('stable')
        b, _ = updates.resolve('beta')
        self.assertEqual((a/'androidarm64/libsteamclient.so').read_bytes(), b'stable')
        self.assertEqual((b/'androidarm64/libsteamclient.so').read_bytes(), b'beta')
        beta.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed'): updates.resolve('beta')
        self.assertFalse(updates.status(remote=False)['beta']['current']['compatible'])
        self.assertEqual(updates.resolve('stable')[0]/'androidarm64/libsteamclient.so', stable)

    def test_missing_beta_never_uses_stable(self):
        self.publish('stable', '20261022.1', '0.4.5')
        with self.assertRaisesRegex(ValueError, 'not installed'): updates.resolve('beta')

    def test_new_bridge_rechecks_previously_incompatible_download_without_replacing_receipt(self):
        client = self.publish('beta', '20261113.6134720', '0.5.9')
        receipt_path = client.parents[2]/'receipt.json'
        receipt = json.loads(receipt_path.read_text())
        receipt.update(compatible=False, compatibility_error='Missing ABI profile')
        updates.write_json(receipt_path, receipt)
        before = receipt_path.read_bytes()
        with patch.object(updates, 'compatibility', return_value=(True, '')) as check:
            root, checked = updates.resolve('beta')
            self.assertEqual(root/'androidarm64/libsteamclient.so', client)
            self.assertTrue(checked['compatible'])
            self.assertEqual(checked['compatibility_error'], '')
            self.assertTrue(updates.status(remote=False)['beta']['current']['compatible'])
            self.assertEqual(check.call_count, 2)
        self.assertEqual(receipt_path.read_bytes(), before)
        with patch.object(updates, 'compatibility', return_value=(False, 'Still unsupported')):
            with self.assertRaisesRegex(ValueError, 'Still unsupported'):
                updates.resolve('beta')
        client.write_bytes(b'tampered')
        with patch.object(updates, 'compatibility') as check:
            with self.assertRaisesRegex(ValueError, 'Missing or changed'):
                updates.resolve('beta')
            check.assert_not_called()

    def test_status_keeps_local_versions_when_one_remote_feed_fails(self):
        self.publish('stable', '20261022.1', '0.4.5')
        def latest(channel):
            if channel == 'beta': raise OSError('offline')
            return updates.candidate(feed(), channel)
        with patch.object(updates, 'latest', side_effect=latest):
            result = updates.status()
        self.assertEqual(result['stable']['current']['version'], '0.4.5')
        self.assertEqual(result['stable']['remote']['version'], '0.4.5')
        self.assertIsNone(result['beta']['remote'])
        self.assertEqual(result['beta']['error'], 'offline')

    def test_untrusted_download_and_redirect_do_not_replace_cached_file(self):
        path = self.root/'download'
        path.write_bytes(b'old')
        with self.assertRaises(ValueError): updates.download('https://other.test/file', path, 20)
        class Response(io.BytesIO):
            url = 'http://steamdeck-images.steamos.cloud/evil'
        with patch.object(updates, 'urlopen', return_value=Response(b'evil')):
            with self.assertRaisesRegex(ValueError, 'redirect'):
                updates.download(updates.IMAGES+'/file', path, 20)
        self.assertEqual(path.read_bytes(), b'old')
        self.assertFalse(path.with_suffix('.part').exists())

    def test_oversized_download_never_publishes_partial_file(self):
        class Response(io.BytesIO):
            url = updates.IMAGES+'/file'
        path = self.root/'download'
        with patch.object(updates, 'urlopen', return_value=Response(b'oversize')):
            with self.assertRaisesRegex(ValueError, 'budget'):
                updates.download(Response.url, path, 3)
        self.assertFalse(path.exists())

    def test_both_committed_valve_keys_match_pinned_fingerprints(self):
        import hashlib
        for name, fingerprint in [('steamdeck-images.pem', updates.CA_FINGERPRINT),
                                  ('steamos-dev-images.pem', updates.DEV_CA_FINGERPRINT)]:
            self.assertEqual(hashlib.sha256(ssl.PEM_cert_to_DER_cert((TOOLS/name).read_text())).hexdigest(), fingerprint)

    def test_bad_signature_is_rejected_before_reading_any_rootfs_metadata(self):
        bundle = self.root/'update.raucb'
        bundle.write_bytes(b'payload' + bytes(64) + (64).to_bytes(8, 'big'))
        with patch.object(updates.subprocess, 'run', side_effect=subprocess.CalledProcessError(4, 'openssl')):
            with self.assertRaises(subprocess.CalledProcessError): updates.verify_bundle(bundle, self.root)
        self.assertFalse((self.root/'rootfs.img.caibx').exists())

    def test_client_profiles_generate_known_session_offsets_and_reject_unknown(self):
        old = '4b1318ea53168ecbf74318ef5b330eb0a41a3e2d335c14f3827504bebba4b40d'
        current = '299135eca46c74987ee19ec145d27d8c6ebfc72af3c4d51eb347a332453cb76c'
        self.assertIn('return offset;', updates.login_header(old))
        header = updates.login_header(current)
        self.assertIn('case 0x1286858: return 0x127b9c0;', header)
        self.assertIn('case 0x10d5704: return 0x10cb954;', header)
        self.assertIn('default: return SIZE_MAX;', header)
        with self.assertRaisesRegex(ValueError, 'ABI profile'): updates.login_header('0'*64)
        beta = updates.login_header('807b50fe4bb34bd7de76180ed4a9ac305185ada8a51be354e93371aeff8a9786')
        self.assertIn('case 0x1286858: return 0x12c0d94;', beta)
        self.assertIn('case 0x10d5704: return 0x110c6d4;', beta)
        self.assertIn('default: return SIZE_MAX;', beta)

    def test_staging_verification_rejects_channel_switch_even_with_identical_client_bytes(self):
        spec = importlib.util.spec_from_file_location('channel_mksteam', TOOLS.parent/'visionos/mksteam.py')
        mksteam = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mksteam)
        sha = '4b1318ea53168ecbf74318ef5b330eb0a41a3e2d335c14f3827504bebba4b40d'
        records = [dict(path='client', sha256=sha)]
        output = self.root/'staged'
        output.mkdir()
        (output/'SteamLoginABI.h').write_text(updates.login_header(sha))
        (output/'SteamHostLogin.swift').write_text('host')
        receipt = dict(channel='stable', release='0.4.5', source_build='20261022.1', target='fixture',
                       profile={}, platform='visionos', inputs=records, artifacts=[])
        (output/'manifest.json').write_text(json.dumps(receipt))
        with patch.object(mksteam, 'OUT', output), patch.object(mksteam, 'inputs', return_value=(None, records)), \
             patch.object(mksteam, 'runtime_source', return_value=(None, dict(version='0.4.5', build='20261022.1'))), \
             patch.object(mksteam, 'profile', return_value={}), patch.object(mksteam, 'host_source', return_value='host'):
            with patch.dict(os.environ, {'KLEPTON_STEAM_CHANNEL': 'stable'}):
                self.assertEqual(mksteam.verify_staged('fixture')['channel'], 'stable')
            with patch.dict(os.environ, {'KLEPTON_STEAM_CHANNEL': 'beta'}):
                with self.assertRaisesRegex(ValueError, 'different channel'): mksteam.verify_staged('fixture')

    def test_failed_update_leaves_previous_channel_and_other_channel_intact(self):
        stable = self.publish('stable', '20261020.1', '0.4.4')
        beta = self.publish('beta', '20261020.2', '0.5.5')
        latest = updates.candidate(feed(), 'stable')
        cache = updates.STORE/'downloads'/latest['build']/'rootfs.img'
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'rootfs')
        image = dict(build=latest['build'], size=6, sha256=updates.digest(cache))
        def download(_, path, limit): path.write_bytes(b'bundle')
        with patch.object(updates, 'latest', return_value=latest), patch.object(updates, 'download', side_effect=download), \
             patch.object(updates, 'verify_bundle', return_value=image), \
             patch.object(updates, 'extract_payload', side_effect=ValueError('broken tar')), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, 'broken tar'): updates.update('stable', 'desync')
        self.assertEqual(updates.resolve('stable')[0]/'androidarm64/libsteamclient.so', stable)
        self.assertEqual(updates.resolve('beta')[0]/'androidarm64/libsteamclient.so', beta)

    def test_successful_update_switches_only_its_pointer_after_validation(self):
        previous = self.publish('stable', '20261020.1', '0.4.4')
        beta = self.publish('beta', '20261020.2', '0.5.5')
        latest = updates.candidate(feed(), 'stable')
        cache = updates.STORE/'downloads'/latest['build']/'rootfs.img'
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'rootfs')
        image = dict(build=latest['build'], size=6, sha256=updates.digest(cache))
        def download(_, path, limit): path.write_bytes(b'bundle')
        def extract(_, work):
            # The old pointer must still be usable while the new build is staged.
            self.assertEqual(updates.resolve('stable')[0]/'androidarm64/libsteamclient.so', previous)
            files = []
            for name in ['libsteamclient.so', 'steamservice.so']:
                path = work/'steam/androidarm64'/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'new')
                files.append(dict(path='androidarm64/'+name, size=3, sha256=updates.digest(path)))
            return files
        with patch.object(updates, 'latest', return_value=latest), patch.object(updates, 'download', side_effect=download), \
             patch.object(updates, 'verify_bundle', return_value=image), patch.object(updates, 'extract_payload', side_effect=extract), \
             patch.object(updates, 'compatibility', return_value=(True, '')), \
             patch.object(steam_runtime, 'elf_inventory', return_value={'architecture': 'aarch64'}), contextlib.redirect_stdout(io.StringIO()):
            updates.update('stable', 'desync')
        root, receipt = updates.resolve('stable')
        self.assertEqual(receipt['version'], '0.4.5')
        self.assertEqual((root/'androidarm64/libsteamclient.so').read_bytes(), b'new')
        self.assertTrue(previous.is_file())
        self.assertEqual(updates.resolve('beta')[0]/'androidarm64/libsteamclient.so', beta)

    def test_hotfix_ui_uses_new_bundle_and_checks_receipt_hashes(self):
        source, output = self.root/'ui', self.root/'out'
        records = []
        for name in ['library.js', 'sp.js', 'chunk~1a96cdf59.js', 'libraries/libraries~00299a408.js']:
            path = source/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(steam_auth_ui.ENTRY if name == 'library.js' else '// bundle')
            records.append(dict(path=name, size=path.stat().st_size, sha256=updates.digest(path)))
        steam_auth_ui.package(source, output, records=records)
        self.assertIn('src="sp.js"', (output/'index.html').read_text())
        self.assertNotIn('chunk~2dcc5aaf7', (output/'index.html').read_text())
        (source/'sp.js').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'pin'): steam_auth_ui.package(source, self.root/'bad', records=records)


    def test_beta_ui_exposes_supported_loader_and_rejects_ambiguous_bootstraps(self):
        source = '// prefix\n' + steam_auth_ui.BETA_ENTRY + '\n// suffix'
        exposed = steam_auth_ui.expose_loader(source)
        self.assertNotIn(steam_auth_ui.BETA_ENTRY, exposed)
        self.assertIn(steam_auth_ui.BETA_LOADER, exposed)
        self.assertTrue(exposed.startswith('// prefix\n'))
        self.assertTrue(exposed.endswith('\n// suffix'))
        for main in ['unknown', source + steam_auth_ui.BETA_ENTRY, source + steam_auth_ui.ENTRY]:
            with self.assertRaisesRegex(ValueError, 'bootstrap'):
                steam_auth_ui.expose_loader(main)


if __name__ == '__main__':
    unittest.main()
