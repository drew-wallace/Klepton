"""Offline tests for the runtime acquisition/inspection gate."""
import importlib.util
import hashlib
import io
import json
from pathlib import Path
import stat
import shutil
import subprocess
import struct
import tempfile
import types
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("steam_runtime", Path(__file__).resolve().parents[2] / "steam/tools/steam_runtime.py")
steam = importlib.util.module_from_spec(spec)
spec.loader.exec_module(steam)
auth_spec = importlib.util.spec_from_file_location("steam_auth_ui", Path(__file__).resolve().parents[2] / "steam/tools/steam_auth_ui.py")
auth_ui = importlib.util.module_from_spec(auth_spec)
auth_spec.loader.exec_module(auth_ui)
compat_spec = importlib.util.spec_from_file_location("steam_sdk_compat", Path(__file__).resolve().parents[2] / "steam/tools/steam_sdk_compat.py")
compat = importlib.util.module_from_spec(compat_spec)
compat_spec.loader.exec_module(compat)
device_spec = importlib.util.spec_from_file_location("steam_probe_device", Path(__file__).resolve().parents[2] / "steam/tools/steam_probe_device.py")
device = importlib.util.module_from_spec(device_spec)
with patch.dict("sys.modules", {"steam_runtime": steam, "steam_auth_ui": auth_ui, "steam_sdk_compat": compat}):
    device_spec.loader.exec_module(device)


def elf_fixture():
    strings = b"\0libc.so\0libsteamclient.so\0eventfd\0CreateInterface\0"
    offsets = {s: strings.index(s.encode()) for s in ("libc.so", "libsteamclient.so", "eventfd", "CreateInterface")}
    symbols = bytes(24) + struct.pack("<IBBHQQ", offsets["eventfd"], 0x12, 0, 0, 0, 0)
    symbols += struct.pack("<IBBHQQ", offsets["CreateInterface"], 0x12, 0, 1, 0x100, 16)
    dynamic = struct.pack("<qQqQqQ", 1, offsets["libc.so"], 14, offsets["libsteamclient.so"], 0, 0)
    rela = struct.pack("<QQq", 0x100, 1027, 0)
    data = bytearray(64)
    sections = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for typ, content, link, entry in [(3, strings, 0, 0), (11, symbols, 1, 24), (6, dynamic, 1, 16), (4, rela, 0, 24)]:
        sections.append((0, typ, 0, 0, len(data), len(content), link, 0, 8, entry))
        data.extend(content)
    shoff = len(data)
    for section in sections:
        data.extend(struct.pack("<IIQQQQIIQQ", *section))
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    data[:64] = struct.pack("<16sHHIQQQIHHHHHH", ident, 3, 183, 1, 0, 0, shoff, 0, 64, 56, 0, 64, len(sections), 0)
    return bytes(data)


class SteamRuntimeTests(unittest.TestCase):
    def test_game_host_c_definitions_are_not_overwritten(self):
        generator = Path(__file__).resolve().parents[2] / 'visionos/gen_xcodeproj.py'
        for enabled in ('0', '1'):
            with self.subTest(enabled=enabled), patch.dict('os.environ', {
                'KLEPTON_TARGET': 'walkabout-57013', 'KLEPTON_STEAM_LOCAL': enabled,
                'KLEPTON_ENTITLEMENTS': '0', 'KLEPTON_TEAM': 'TESTTEAM00',
            }), patch.dict('sys.modules', {
                'mksteam': types.SimpleNamespace(verify_staged=lambda: None),
            }):
                project_spec = importlib.util.spec_from_file_location('fixture_project', generator)
                project = importlib.util.module_from_spec(project_spec)
                project_spec.loader.exec_module(project)
                self.assertEqual(project.COMMON.count('GCC_PREPROCESSOR_DEFINITIONS ='), 1)
                self.assertIn('KL_TARGET_DEFAULT=', project.COMMON)
                self.assertIn('"$(inherited)"', project.COMMON)
                self.assertEqual('"KL_STEAM_GAME_HOST=1"' in project.COMMON, enabled == '1')

    def test_existing_observation_verifies_process_and_does_not_launch_or_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); out = root/'probe'; out.mkdir()
            identity = {'pid': 123, 'executable': '/fixture/Probe.app/Probe', 'started': 'fixture start'}
            receipt = out/'receipt.json'
            receipt.write_text(json.dumps({'probe_run_id': 'same-run', 'simulator_process': identity}))
            observed = {'run_id_verified': True, 'probe_returned': False, 'logged_on': False}
            with patch.object(device, 'ROOT', root), \
                 patch.object(device, 'probe_process_identity', return_value=identity), \
                 patch.object(device, 'verify_installed_probe') as verify, \
                 patch.object(device, 'simulator_observation', return_value=observed) as collect, \
                 patch.object(device, 'run') as mutation:
                saved = device.observe_existing_probe(receipt, 'fixture-simulator', 12)
            verify.assert_called_once()
            collect.assert_called_once()
            self.assertEqual(collect.call_args.args[2], 'same-run')
            mutation.assert_not_called()
            snapshot = json.loads(saved.read_text())
            self.assertEqual(snapshot['observation_source'], 'existing_verified_launch')
            self.assertTrue(snapshot['process_live_after'])
            self.assertTrue(snapshot['observation_timed_out'])
            self.assertFalse(snapshot['observed']['logged_on'])
            self.assertNotIn('observed', json.loads(receipt.read_text()))

    def test_existing_observation_rejects_reused_missing_or_unrecorded_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); identity = {'pid': 123, 'executable': '/fixture/Probe', 'started': 'old start'}
            receipt = root/'receipt.json'
            for recorded, actual in [(identity, None), (identity, dict(identity, started='new start')), (None, identity)]:
                receipt.write_text(json.dumps({'simulator_process': recorded}))
                with patch.object(device, 'ROOT', root), \
                     patch.object(device, 'probe_process_identity', return_value=actual), \
                     patch.object(device, 'simulator_observation') as collect, \
                     patch.object(device, 'run') as mutation:
                    with self.assertRaises(ValueError): device.observe_existing_probe(receipt, 'fixture')
                collect.assert_not_called(); mutation.assert_not_called()

    def test_existing_observation_rejects_stale_launch_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); identity = {'pid': 123, 'executable': '/fixture/Probe', 'started': 'old start'}
            receipt = root/'receipt.json'
            receipt.write_text(json.dumps({'probe_run_id': 'same-run', 'simulator_process': identity}))
            with patch.object(device, 'ROOT', root), \
                 patch.object(device, 'probe_process_identity', return_value=identity), \
                 patch.object(device, 'verify_installed_probe'), \
                 patch.object(device, 'simulator_observation', return_value={'run_id_verified': False}):
                with self.assertRaisesRegex(ValueError, 'matching launch log'):
                    device.observe_existing_probe(receipt, 'fixture')

    def test_existing_observation_checks_installed_app_and_framework_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp)/(device.NAME+'.app'); app.mkdir()
            executable = app/device.NAME; executable.write_bytes(b'app')
            debug = app/(device.NAME+'.debug.dylib'); debug.write_bytes(b'debug code')
            framework = app/'Frameworks/libsteamclient.framework'; framework.mkdir(parents=True)
            image = framework/'libsteamclient'; image.write_bytes(b'client code')
            identity = {'executable': str(executable)}
            receipt = {'app_binary_sha256': hashlib.sha256(debug.read_bytes()).hexdigest(),
                       'embedded_client_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}
            with patch.object(device.subprocess, 'check_output', return_value=str(app)):
                device.verify_installed_probe('fixture', receipt, identity)
                debug.write_bytes(b'changed code')
                with self.assertRaisesRegex(ValueError, 'installed probe differs'):
                    device.verify_installed_probe('fixture', receipt, identity)
                debug.write_bytes(b'debug code'); image.write_bytes(b'changed client')
                with self.assertRaisesRegex(ValueError, 'installed framework differs'):
                    device.verify_installed_probe('fixture', receipt, identity)

    @unittest.skipUnless(shutil.which('node'), 'Node is required for offline Valve UI adapter tests')
    def test_qr_adapter_cancellation_and_approved_handoff(self):
        subprocess.run(['node', 'steam/tests/test_steam_auth_ui_login.cjs'], cwd=Path(__file__).resolve().parents[2], check=True)

    def test_sdk_bootstrap_correction_rejects_unknown_input(self):
        with self.assertRaisesRegex(ValueError, 'audited original hash'):
            compat.correct_bootstrap(elf_fixture())

    def test_sdk_bootstrap_correction_changes_only_mapped_instruction(self):
        # Synthetic ELF has different file/virtual addresses, just like a real
        # packaged library. No proprietary binary is required by this test.
        data = bytearray(256)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 192, compat.ADDRESS - 8, 0, 32, 32, 4)
        struct.pack_into('<I', data, 200, compat.ORIGINAL)
        original = bytes(data)
        with patch.object(compat, 'SHA256', hashlib.sha256(original).hexdigest()):
            corrected, receipt = compat.correct_bootstrap(original)
        self.assertEqual(receipt['file_offset'], 200)
        self.assertEqual(corrected[:200], original[:200])
        self.assertEqual(corrected[204:], original[204:])
        self.assertEqual(struct.unpack_from('<I', corrected, 200)[0], compat.REPLACEMENT)
        self.assertEqual(receipt['translation_input_sha256'], hashlib.sha256(corrected).hexdigest())
        # Even an attested image must still have the audited instruction.
        struct.pack_into('<I', data, 200, 0)
        wrong_instruction = bytes(data)
        with patch.object(compat, 'SHA256', hashlib.sha256(wrong_instruction).hexdigest()):
            with self.assertRaisesRegex(ValueError, 'instruction mismatch'):
                compat.correct_bootstrap(wrong_instruction)

    def test_elf_dependencies_and_capabilities(self):
        r = steam.elf_inventory(elf_fixture())
        self.assertEqual(r["architecture"], "aarch64")
        self.assertEqual(r["needed"], ["libc.so"])
        self.assertEqual(r["soname"], "libsteamclient.so")
        self.assertEqual(r["entry_points"], ["CreateInterface"])
        self.assertEqual(r["imports"], [{"name": "eventfd", "weak": False}])
        self.assertEqual(r["imported_capabilities"]["event_sync"], ["eventfd"])
        self.assertEqual(r["relocation_types"], {"1027": 1})

    def test_truncated_elf_rejected(self):
        with self.assertRaises(ValueError):
            steam.elf_inventory(elf_fixture()[:-1])
        with self.assertRaises(ValueError):
            steam.elf_inventory(b"\x7fELF\x02\x01")
        self.assertIsNone(steam.elf_inventory(b"not an ELF"))

    def test_wrong_architecture_is_visible(self):
        data = bytearray(elf_fixture())
        struct.pack_into("<H", data, 18, 62)
        self.assertEqual(steam.elf_inventory(bytes(data))["architecture"], "x86_64")

    def test_cached_artifact_hash_and_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "artifact"
            path.write_bytes(b"pinned")
            url = f"https://{steam.CDN}/artifact"
            self.assertEqual(steam.fetch_file(url, path, steam.sha256(b"pinned"), 6), b"pinned")
            for size, digest in [(7, steam.sha256(b"pinned")), (6, "0" * 64)]:
                with self.assertRaises(ValueError):
                    steam.fetch_file(url, path, digest, size)
            with self.assertRaises(ValueError):
                steam.fetch_file("http://example.com/artifact", path, steam.sha256(b"pinned"), 6)

    def archive(self, tmp, entries):
        path = Path(tmp) / "archive.zip"
        with zipfile.ZipFile(path, "w") as z:
            for name, value, mode in entries:
                item = zipfile.ZipInfo(name)
                item.external_attr = mode << 16
                z.writestr(item, value)
        return path

    def test_windows_paths_and_contained_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = self.archive(tmp, [("bin\\client.so", b"ELF", stat.S_IFREG | 0o644),
                                         ("bin\\alias.so", b"client.so", stat.S_IFLNK | 0o777)])
            dest = Path(tmp) / "out"
            steam.extract(archive, dest)
            steam.extract(archive, dest)  # reproducible re-extraction
            self.assertEqual((dest / "bin/alias.so").read_bytes(), b"ELF")

    def test_archive_traversal_rejected_before_writing(self):
        for name, value, mode in [("../outside", b"bad", stat.S_IFREG),
                                  ("/absolute", b"bad", stat.S_IFREG),
                                  ("link", b"../outside", stat.S_IFLNK)]:
            with tempfile.TemporaryDirectory() as tmp:
                archive = self.archive(tmp, [("first", b"ok", stat.S_IFREG), (name, value, mode)])
                dest = Path(tmp) / "out"
                with self.assertRaises(ValueError):
                    steam.extract(archive, dest)
                self.assertFalse((dest / "first").exists())

    def test_inventory_reads_verified_archives_and_never_passes_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = self.archive(tmp, [("androidarm64/libsteamclient.so", elf_fixture(), stat.S_IFREG)])
            data = archive.read_bytes()
            lock = {"client_version": "123", "manifest_sha256": "0" * 64,
                    "packages": [{"name": "client", "archive": archive.name,
                                  "size": len(data), "sha256": steam.sha256(data)}]}
            result = steam.inventory(lock, root, None)
            self.assertEqual(result["runtime_gate"]["status"], "unproven")
            self.assertEqual(len(result["artifacts"]), 1)
            archive.write_bytes(b"corrupt")
            with self.assertRaises(ValueError):
                steam.inventory(lock, root, None)

    def test_committed_lock_has_pinned_valve_urls(self):
        lock = json.loads(steam.DEFAULT_LOCK.read_text())
        self.assertEqual(lock["schema_version"], 1)
        steam.manifest_snapshot(lock, steam.HERE)
        for p in lock["packages"]:
            self.assertTrue(p["url"].startswith(f"https://{steam.CDN}/"))
            self.assertEqual(len(p["sha256"]), 64)
            self.assertGreater(p["size"], 0)

    def test_lock_cannot_select_packages_outside_the_saved_manifest(self):
        lock = json.loads(steam.DEFAULT_LOCK.read_text())
        lock["packages"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "package metadata"):
            steam.manifest_snapshot(lock, steam.HERE)

    def test_probe_records_failure_and_never_marks_mapping_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = root / "probe"
            binary.write_bytes(b"test placeholder")
            known = []
            for package, relative in [("bins_androidarm64_linuxarm64", "androidarm64/libsteamclient.so"),
                                      ("bins_sdk_linuxarm64_linuxarm64", "linuxarm64/steamclient.so")]:
                lib = root / "artifacts" / package / relative
                lib.parent.mkdir(parents=True)
                lib.write_bytes(b"pinned")
                known.append({"package": package, "path": relative, "size": 6, "sha256": steam.sha256(b"pinned")})
            lock = {"client_version": "123", "manifest_sha256": "0" * 64}
            with patch.object(steam, "inventory", return_value={"artifacts": known}), patch.object(steam.subprocess, "run") as run:
                run.return_value.returncode = 4
                result = steam.probe(lock, root, binary)
                self.assertEqual(result["runtime_gate"]["status"], "unproven")
                self.assertEqual(result["runtime_gate"]["ticket_validation"], "not_run")
                run.side_effect = steam.subprocess.TimeoutExpired("probe", 1)
                result = steam.probe(lock, root, binary)
                self.assertEqual(result["runtime_gate"]["status"], "blocked")
                self.assertTrue(all(r["timed_out"] for r in result["runs"]))

    def test_simulator_receipt_rejects_finished_log_from_previous_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root/'Library/Application Support/steam-probe.log'
            log.parent.mkdir(parents=True)
            log.write_text('[steam-probe] run id: old-run\n'
                           '[steam-probe] SteamUser023: present\n'
                           '[steam-probe] runtime gate UNPROVEN; no login\n')
            out = root/'receipt'; out.mkdir()
            with patch.object(device.subprocess, 'check_output', return_value=str(root)), \
                 patch.object(device.time, 'monotonic', side_effect=[0, 0, 21]), \
                 patch.object(device.time, 'sleep'):
                observed = device.simulator_observation('fixture', out, 'new-run')
            self.assertFalse(observed['probe_returned'])
            self.assertFalse(observed['genuine_user_interface'])
            self.assertIsNone(observed['callback_count'])
            self.assertEqual((out/'steam-probe.log').read_text(), '')

    def test_interactive_receipt_records_readiness_without_claiming_login_or_shutdown(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root/'Library/Application Support/steam-probe.log'
            log.parent.mkdir(parents=True)
            log.write_text('[steam-probe] run id: current-run\n'
                           '[steam-probe] interactive native session ready; login_required=1\n'
                           '[steam-probe] interactive Valve auth stage=modules_ready result=0\n')
            out = root/'receipt'; out.mkdir()
            with patch.object(device.subprocess, 'check_output', return_value=str(root)), \
                 patch.object(device.time, 'monotonic', return_value=0):
                observed = device.simulator_observation('fixture', out, 'current-run', interactive=True)
            self.assertTrue(observed['interactive_backend_ready'])
            self.assertTrue(observed['interactive_ui_ready'])
            self.assertFalse(observed['probe_returned'])
            self.assertFalse(observed['interactive_native_logged_on'])
            self.assertFalse(observed['interactive_poll_returned_success'])
            self.assertFalse(observed['shutdown_returned_true'])

    def test_sdk_receipt_preserves_failed_attempt_and_latest_result_without_inventing_ticket_delivery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); log = root/'Library/Application Support/steam-probe.log'
            log.parent.mkdir(parents=True)
            log.write_text('[steam-probe] run id: sdk-run\n'
                           '[steam-probe] Walkabout genuine SDK loaded; requesting real initialization\n'
                           '[steam-probe] Walkabout client instance distinct=1\n'
                           '[steam-probe] Walkabout SteamAPI_InitFlat result=3 reason_connect_global=1 reason_app_context=0\n'
                           '[steam-probe] Walkabout SteamAPI_InitFlat result=0\n'
                           '[steam-probe] Walkabout SDK interfaces present; app_id=1408230 pipe=3 user=1\n'
                           '[steam-probe] interactive native BLoggedOn=1\n'
                           '[steam-probe] interactive native BLoggedOn=0\n'
                           '[steam-probe] runtime gate UNPROVEN; fixture\n')
            out = root/'receipt'; out.mkdir()
            with patch.object(device.subprocess, 'check_output', return_value=str(root)), \
                 patch.object(device.time, 'monotonic', return_value=0):
                observed = device.simulator_observation('fixture', out, 'sdk-run')
            self.assertEqual(observed['walkabout_sdk_init_attempts'], [3, 0])
            self.assertEqual(observed['walkabout_sdk_init_result'], 0)
            self.assertTrue(observed['walkabout_client_instance_distinct'])
            self.assertTrue(observed['walkabout_sdk_connect_global_failure_observed'])
            self.assertFalse(observed['walkabout_sdk_app_context_failure_observed'])
            self.assertEqual(observed['walkabout_sdk_app_id'], 1408230)
            self.assertFalse(observed['walkabout_ticket_delivery_observed'])
            self.assertFalse(observed['interactive_native_logged_on'])
            self.assertFalse(observed['logged_on'])

    def test_helper_receipt_does_not_confuse_server_pointer_with_account_or_ipc_readiness(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); log = root/'Library/Application Support/steam-probe.log'
            log.parent.mkdir(parents=True)
            out = root/'receipt'; out.mkdir()
            prefix = ('[steam-probe] run id: helper-run\n'
                      '[steam-probe] service constructors returned\n'
                      '[steam-probe] service StartThread returned; server_present=1 getter_matches=1 threads_created=1\n')
            for suffix, stopped in [('', False), ('[steam-probe] service Stop returned\n[steam-probe] service Shutdown returned\n', True)]:
                log.write_text(prefix + suffix + '[steam-probe] runtime gate UNPROVEN; fixture\n')
                with patch.object(device.subprocess, 'check_output', return_value=str(root)), \
                     patch.object(device.time, 'monotonic', return_value=0):
                    observed = device.simulator_observation('fixture', out, 'helper-run')
                self.assertTrue(observed['service_server_present'])
                self.assertTrue(observed['service_server_getter_matches'])
                self.assertEqual(observed['service_threads_created'], 1)
                self.assertEqual(observed['service_stop_returned'], stopped)
                self.assertEqual(observed['service_shutdown_returned'], stopped)
                self.assertFalse(observed['service_ipc_request_tested'])
                self.assertFalse(observed['local_backend_created'])
                self.assertFalse(observed['genuine_user_interface'])
                self.assertFalse(observed['logged_on'])
                self.assertFalse(observed['walkabout_ticket_delivery_observed'])

    def test_simulator_receipt_waits_for_matching_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root/'Library/Application Support/steam-probe.log'
            log.parent.mkdir(parents=True)
            log.write_text('[steam-probe] run id: old-run\n'
                           '[steam-probe] runtime gate UNPROVEN; old\n')
            out = root/'receipt'; out.mkdir()
            def fresh_log(_):
                log.write_text('[steam-probe] run id: new-run\n'
                               '[steam-probe] native login ABI verified; state=0\n'
                               '[steam-probe] credential-free Steam_LogOn result=5 state=0\n'
                               '[steam-probe] frame pump returned; callbacks=11 IPC calls=19\n'
                               '[steam-probe] WebUI clientdll loopback reachable=1\n'
                               '[steam-probe] minimal WebSocket HTTP status=403\n'
                               '[steam-probe] WebSocket HTTP status=403 error_code=-1011 timeout=false\n'
                               '[steam-probe] Valve auth UI stage=modules_ready result=0 challenge_present=0\n'
                               '[steam-probe] Valve auth UI stage=qr_begin_returned result=1 challenge_present=1 transport_error=1\n'
                               '[steam-probe] BShutdownIfAllPipesClosed: 1\n'
                               '[steam-probe] runtime gate UNPROVEN; no login\n')
            with patch.object(device.subprocess, 'check_output', return_value=str(root)), \
                 patch.object(device.time, 'monotonic', return_value=0), \
                 patch.object(device.time, 'sleep', side_effect=fresh_log):
                observed = device.simulator_observation('fixture', out, 'new-run')
            self.assertTrue(observed['probe_returned'])
            self.assertTrue(observed['native_login_abi_verified'])
            self.assertEqual(observed['credential_free_login_result'], 5)
            self.assertEqual(observed['callback_count'], 11)
            self.assertEqual(observed['ipc_calls'], 19)
            self.assertTrue(observed['shutdown_returned_true'])
            self.assertFalse(observed['logged_on'])
            self.assertTrue(observed['webui_loopback_reachable'])
            self.assertEqual(observed['webui_websocket_http_status'], 403)
            self.assertEqual(observed['webui_minimal_http_status'], 403)
            self.assertFalse(observed['webui_websocket_upgraded'])
            self.assertTrue(observed['auth_ui_modules_ready'])
            self.assertEqual(observed['auth_ui_qr_result'], 1)
            self.assertTrue(observed['auth_ui_challenge_present'])
            self.assertFalse(observed['logged_on'])  # QR challenge is not account authentication.

    def test_auth_ui_artifact_mismatch_rejected_before_packaging(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root/'source'; source.mkdir()
            (source/'library.js').write_bytes(b'changed')
            lock = root/'lock.json'
            lock.write_text(json.dumps({'files': [{'path': 'library.js', 'size': 6,
                                                   'sha256': steam.sha256(b'pinned')}]}))
            with patch.object(auth_ui, 'LOCK', lock), self.assertRaisesRegex(ValueError, 'does not match'):
                auth_ui.package(source, root/'output')
            self.assertFalse((root/'output').exists())

    def test_auth_ui_unknown_bootstrap_rejected_before_packaging(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root/'source'; source.mkdir()
            content = b'unknown desktop entry'
            (source/'library.js').write_bytes(content)
            lock = root/'lock.json'
            lock.write_text(json.dumps({'files': [{'path': 'library.js', 'size': len(content),
                                                   'sha256': steam.sha256(content)}]}))
            with patch.object(auth_ui, 'LOCK', lock), self.assertRaisesRegex(ValueError, 'Unrecognized'):
                auth_ui.package(source, root/'output')
            self.assertFalse((root/'output').exists())

    def test_missing_artifacts_fail_without_a_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "missing pinned archive"):
                steam.inventory({"packages": [{"archive": "absent.zip"}]}, Path(tmp), None)


if __name__ == "__main__":
    unittest.main()
