#!/usr/bin/env python3
"""Klepton APK Installer backend. stdout is versioned NDJSON, never tool output.

  python3 tools/utility.py doctor
  python3 tools/utility.py inspect --apk /path/game.apk
  python3 tools/utility.py run --request /path/request.json

The request schema and recovery behavior are documented in docs/INSTALLER.md.
Only the Python standard library is required beyond Klepton's existing tools.
"""
from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import selectors
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from urllib.parse import unquote, urlparse
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / 'build/apk-installer'
sys.path.insert(0, str(ROOT / 'visionos'))
sys.path.insert(0, str(ROOT / 'steam/tools'))
import targets
import steam_updates
import steam_library

ANDROID = '{http://schemas.android.com/apk/res/android}'
STEAM_PROFILES = {
    'walkabout-57013': ROOT / 'games/walkabout/steam/walkabout_steam_api.lock.json',
    'eleven': ROOT / 'games/eleven/steam/steam_api.lock.json',
}
KINDS = ('unity', 'ue4', 'native', 'steamlink', 'jkxr', 'sdl2', 'gles3jni')
RETAIL_PATHS = {'jkxr_ja': 'JKXR/JK3/base', 'jkxr_jo': 'JKXR/JK2/base'}
KNOWN_RUNTIME_LIMITATIONS = {
    '6cae96955397be3522c238196635a061c5f9da465f032cbe34b817dddbb4a18a':
        'Known Bigscreen limitation: Klepton now includes an experimental WebKit bridge for Vuplex. '
        'Simulator startup and browser textures work. Automatic Oculus login requires Meta account services, '
        'which Klepton does not currently provide; a WebView or ovrport cannot supply a signed-in platform account. '
        'Email login, saved sessions, ordinary video playback and Lobby joins have been tested in the simulator. '
        'Advanced → GLES compatibility → GLES 3.1 enables avatar color buffers; Unity CPU skinning avoids the unsupported compute-skinning path. '
        'Use Collect Headset Logs to investigate remaining failures; ovrport is not required for this bridge.',
}


class UtilityError(Exception):
    pass


class Cancelled(UtilityError):
    pass


def emit(stage, status, message='', **data):
    print(json.dumps(dict(schema_version=1, stage=stage, status=status,
                          message=message, **data)), flush=True)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n')
    temporary.replace(path)


@contextlib.contextmanager
def checkout_lock():
    STORE.mkdir(parents=True, exist_ok=True)
    STORE.chmod(0o700)  # Backups can contain saved sessions and account data.
    with (STORE / 'job.lock').open('a') as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise UtilityError('Another APK Installer job is using this checkout. Wait for it or cancel it.')
        try:
            yield
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


class Runner:
    """Own every subprocess group; drain pipes without blocking UI cancellation."""
    def __init__(self, directory, verbose=False):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.log_path = self.directory / 'build.log'
        self.verbose = verbose
        self.stage = 'inspect'
        self.cancelled = False
        self.child = None
        self.previous_handlers = {}
        self.recovery_job = ''

    def install_handlers(self):
        for signum in (signal.SIGTERM, signal.SIGINT):
            self.previous_handlers[signum] = signal.signal(signum, self.cancel)

    def restore_handlers(self):
        for signum, handler in self.previous_handlers.items():
            signal.signal(signum, handler)

    def cancel(self, *_):
        self.cancelled = True
        self.stop_child()

    def stop_child(self):
        if self.child is not None:
            try:
                os.killpg(self.child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def check_cancelled(self):
        if self.cancelled:
            raise Cancelled('Cancelled. Partial output and backups were retained; installation is not complete.')

    def start(self, stage, message):
        self.check_cancelled()
        self.stage = stage
        emit(stage, 'running', message, log_path=str(self.log_path))

    def run(self, argv, env=None, timeout=None):
        self.check_cancelled()
        argv = [str(a) for a in argv]
        with self.log_path.open('ab') as log:
            log.write(('\n$ ' + shlex.join(argv) + '\n').encode())
            log.flush()
            child = subprocess.Popen(argv, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            self.child = child
            selector = selectors.DefaultSelector()
            selector.register(child.stdout, selectors.EVENT_READ)
            start = time.monotonic()
            pending = b''
            try:
                while selector.get_map():
                    if self.cancelled:
                        self.stop_child()
                        if time.monotonic() - start > 2:
                            try:
                                os.killpg(child.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                    if timeout and time.monotonic() - start > timeout:
                        self.stop_child()
                        try:
                            child.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            os.killpg(child.pid, signal.SIGKILL)
                        raise UtilityError(f'{argv[0]} timed out. Check the connection and retry; see {self.log_path}.')
                    for key, _ in selector.select(0.2):
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        log.write(chunk)
                        log.flush()
                        if self.verbose:
                            pending += chunk
                            lines = pending.split(b'\n')
                            pending = lines.pop()
                            for line in lines:
                                emit(self.stage, 'log', line.decode('utf-8', errors='replace'))
                code = child.wait()
            finally:
                selector.close()
                child.stdout.close()
                self.stop_child()  # includes grandchildren which inherited a pipe
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                self.child = None
            self.check_cancelled()
            if code:
                tail = self.log_path.read_bytes()[-2400:].decode('utf-8', errors='replace')
                if 'No Account for Team' in tail or 'No profiles for' in tail:
                    raise UtilityError('Xcode could not provision the selected team and bundle identifier. '
                                       'Verify the selected signing team and check its account/profile status '
                                       'under Xcode → Settings → Accounts, then retry. '
                                       f'No app was installed. Details: {self.log_path}')
                raise UtilityError(f'{self.stage} failed (exit {code}). See {self.log_path}.\n{tail}')

    def dev_json(self, args):
        path = self.directory / ('device-' + uuid.uuid4().hex + '.json')
        self.run(['xcrun', 'devicectl', *args, '--timeout', '30', '--json-output', path], timeout=40)
        data = json.loads(path.read_text())
        if data.get('info', {}).get('outcome') != 'success':
            raise UtilityError(f'CoreDevice could not complete {self.stage}. Unlock the Vision Pro and retry.')
        return data['result']


def capture(args, timeout=15, input_text=None):
    try:
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=timeout, input=input_text)
        return result.stdout if result.returncode == 0 else ''
    except (OSError, subprocess.SubprocessError):
        return ''


def discover_devices(data):
    result = []
    for d in data.get('result', {}).get('devices', []):
        props = d.get('properties', {})
        hardware = props.get('hardware', d.get('hardwareProperties', {}))
        state = props.get('state', d.get('deviceProperties', {}))
        connection = props.get('connection', d.get('connectionProperties', {}))
        if hardware.get('platform') != 'visionOS' or hardware.get('reality') != 'physical':
            continue
        result.append(dict(id=d['identifier'], name=state.get('name', 'Vision Pro'),
                           state=connection.get('state', connection.get('tunnelState', 'unknown')),
                           paired=connection.get('pairingState') == 'paired',
                           developer_mode=state.get('developerModeStatus', 'unknown')))
    return result


def signing_teams():
    """Match usable keychain identities to certificate OU, never the CN suffix.

    The parenthesized identifier in an Apple Development display name belongs
    to the certificate's person and may differ from its development team.
    """
    identities = capture(['security', 'find-identity', '-v', '-p', 'codesigning'])
    valid = {fingerprint.upper(): name for fingerprint, name in re.findall(
        r'(?m)^\s*\d+\)\s+([A-Fa-f0-9]{40})\s+"(Apple Development: [^"]+)"', identities)}
    certificates = capture(['security', 'find-certificate', '-a', '-c', 'Apple Development', '-p'])
    teams = []
    for pem in re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', certificates, re.S):
        details = capture(['openssl', 'x509', '-noout', '-subject', '-fingerprint', '-sha1',
                           '-nameopt', 'RFC2253'], input_text=pem+'\n')
        fingerprint = re.search(r'(?im)^sha1 Fingerprint=([0-9a-f:]+)\s*$', details)
        subject = re.search(r'(?m)^subject=(.*)$', details)
        if not fingerprint or not subject:
            continue
        identity = valid.get(fingerprint[1].replace(':', '').upper())
        team = re.search(r'(?:^|,)OU=([A-Z0-9]{10})(?:,|$)', subject[1])
        if identity and team and team[1] not in [t['id'] for t in teams]:
            teams.append(dict(id=team[1], name=f'Team {team[1]} · {identity}'))
    return teams


def doctor():
    checks = []
    def check(name, ready, fix, required=True):
        checks.append(dict(name=name, ready=bool(ready), required=required, fix='' if ready else fix))
    check('Apple Silicon Mac', sys.platform == 'darwin' and platform.machine() == 'arm64',
          'Use an Apple Silicon Mac with full Xcode installed.')
    for name in ('python3', 'apktool', 'make', 'xcodebuild', 'codesign'):
        check(name, shutil.which(name), 'brew install apktool' if name == 'apktool' else
              'Install Xcode and select it with: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer')
    check('visionOS SDK', capture(['xcrun', '--sdk', 'xros', '--show-sdk-path']),
          'Install the visionOS SDK in Xcode → Settings → Components.')
    check('ANGLE device renderer', (ROOT/'vendor/out/xros/libEGL.framework/libEGL').is_file()
          and (ROOT/'vendor/out/xros/libGLESv2.framework/libGLESv2').is_file(), 'make angle-all')
    check('MoltenVK', (ROOT/'vendor-moltenvk/out/xros/MoltenVK.xcframework').exists()
          or any((ROOT/'vendor-moltenvk/out/xros').glob('**/MoltenVK')), 'make mvk', required=False)
    teams = signing_teams()
    check('Development signing identity', teams, 'Add an Apple account and development certificate in Xcode → Settings → Accounts.')
    devices = []
    with tempfile.TemporaryDirectory(prefix='klepton-devices-') as tmp:
        path = Path(tmp)/'devices.json'
        capture(['xcrun', 'devicectl', 'list', 'devices', '--json-output', str(path)], timeout=20)
        if path.is_file():
            try:
                devices = discover_devices(json.loads(path.read_text()))
            except (ValueError, KeyError):
                pass
    return dict(checks=checks, devices=devices, teams=teams,
                profiles=[dict(id=n, name=t['display'], kind=t['kind'], entry=t['entry'],
                               obb=t['obb'], data_path=RETAIL_PATHS.get(n, t.get('stage_files', '')),
                               steam=n in STEAM_PROFILES, bundle=targets.bundle_for(n))
                          for n,t in sorted(targets.TARGETS.items())],
                ready=all(c['ready'] for c in checks if c['required']))


def archive_inventory(apk):
    if Path(apk).suffix.lower() != '.apk':
        raise UtilityError('Choose a single .apk. AAB, XAPK, APKM and split packages are not supported.')
    try:
        with zipfile.ZipFile(apk) as z:
            names = set()
            libraries = []
            expanded = 0
            for item in z.infolist():
                p = PurePosixPath(item.filename)
                if (p.is_absolute() or '..' in p.parts or '\\' in item.filename or
                        stat.S_ISLNK(item.external_attr >> 16) or item.flag_bits & 1):
                    raise UtilityError('APK contains an unsafe path, symbolic link, or encrypted entry.')
                if item.filename in names:
                    raise UtilityError('APK contains duplicate ZIP entries.')
                names.add(item.filename)
                expanded += item.file_size
                if p.suffix.lower() == '.apk':
                    raise UtilityError('This archive contains split APKs. Choose a standalone APK.')
                if len(p.parts) == 3 and p.parts[:2] == ('lib', 'arm64-v8a') and p.suffix == '.so':
                    with z.open(item) as f:
                        header = f.read(64)
                    if header[:4] == b'\x7fELF':
                        if len(header) < 64 or header[4:6] != b'\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
                            raise UtilityError(f'{p.name} is not a little-endian ARM64 ELF library.')
                        if not re.fullmatch(r'lib[A-Za-z0-9_+.-]+\.so', p.name):
                            raise UtilityError('APK contains a native-library name unsupported by the build tools.')
                        libraries.append(p.stem)
            if 'AndroidManifest.xml' not in names:
                raise UtilityError('APK has no AndroidManifest.xml.')
            if not libraries:
                raise UtilityError('APK has no ARM64 native libraries. Klepton cannot run Java/JVM-only or x86/ARM32 apps.')
            return sorted(libraries), expanded
    except (zipfile.BadZipFile, RuntimeError) as e:
        raise UtilityError(f'Invalid APK archive: {e}') from e


def yaml_value(text, key):
    match = re.search(r'^\s*'+re.escape(key)+r':\s*(.+)$', text, re.M)
    if not match:
        return ''
    value = match[1].strip()
    if value.startswith('"'):
        try:
            return str(json.loads(value))
        except ValueError:
            return ''
    return value.strip("'")


def decoded_metadata(tree, libraries):
    try:
        manifest = ET.parse(Path(tree)/'AndroidManifest.xml').getroot()
    except (OSError, ET.ParseError) as e:
        raise UtilityError('Could not read the decoded manifest; inspect the decode log.') from e
    if manifest.get('split') or any(x.tag == 'uses-split' for x in manifest):
        raise UtilityError('Split APKs are not supported; choose a standalone APK.')
    package = manifest.get('package', '')
    if not re.fullmatch(r'[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+', package):
        raise UtilityError('APK has an invalid or missing package identifier.')
    version_text = (Path(tree)/'apktool.yml').read_text() if (Path(tree)/'apktool.yml').exists() else ''
    app = manifest.find('application')
    label = app.get(ANDROID+'label', '') if app is not None else ''
    if label.startswith('@string/'):
        resource = Path(tree)/'res/values/strings.xml'
        if resource.exists():
            strings = ET.parse(resource).getroot()
            label = next((''.join(x.itertext()) for x in strings if x.get('name') == label[8:]), '')
    if not label or label.startswith('@'):
        label = package.rsplit('.', 1)[-1]
    activities = [] if app is None else list(app.findall('activity'))
    kinds = []
    if {'libmain', 'libunity'}.issubset(libraries):
        kinds.append(('unity', 'libmain'))
    if 'libUE4' in libraries:
        kinds.append(('ue4', 'libUE4'))
    for activity in activities:
        if activity.get(ANDROID+'name') == 'android.app.NativeActivity':
            for meta in activity.findall('meta-data'):
                if meta.get(ANDROID+'name') == 'android.app.lib_name':
                    entry = meta.get(ANDROID+'value', '')
                    entry = entry if entry.startswith('lib') else 'lib'+entry
                    if entry in libraries:
                        kinds.append(('native', entry))
    kinds = list(dict.fromkeys(kinds))
    kind, entry = kinds[0] if len(kinds) == 1 else ('', '')
    obb_required = any(m.get(ANDROID+'name', '').endswith('bPackageDataInsideApk') and
                       m.get(ANDROID+'value', '').lower() == 'false' for m in manifest.iter('meta-data'))
    # Unity's expansion flag is in assets, not consistently in the manifest.
    settings = Path(tree)/'assets/bin/Data/settings.xml'
    if settings.exists():
        obb_required |= bool(re.search(r'<bool name="useObb">\s*True\s*</bool>', settings.read_text(errors='replace'), re.I))
    graphics = 'Vulkan + GLES (detected libraries/manifest)' if 'vulkan' in ET.tostring(manifest).decode().lower() else 'Automatic (ANGLE / MoltenVK)'
    vulkan_required = any(f.get(ANDROID+'name', '').startswith('android.hardware.vulkan') and
                          f.get(ANDROID+'required', 'true') != 'false' for f in manifest.findall('uses-feature'))
    return dict(package=package, name=label[:160], version_code=yaml_value(version_text, 'versionCode'),
                version_name=yaml_value(version_text, 'versionName'), architecture='ARM64',
                kind=kind, entry=entry, libraries=libraries, steam='libsteam_api' in libraries,
                obb_required=obb_required, graphics=graphics, vulkan_required=vulkan_required)


def profile_catalog():
    """Committed fingerprints remain useful when other proprietary APKs are absent."""
    path = ROOT/'tools/apk_profiles.json'
    return json.loads(path.read_text()) if path.exists() else {}


def match_profiles(info, tree, catalog=None):
    matches = []
    catalog = profile_catalog() if catalog is None else catalog
    exact = [name for name, p in catalog.items() if p.get('apk_sha256') == info.get('apk_sha256') and p.get('apk_sha256')]
    if exact:
        return sorted(exact)
    for name, p in catalog.items():
        if p['package'] != info['package'] or str(p['version_code']) != info['version_code']:
            continue
        binary = Path(tree)/'lib/arm64-v8a'/(targets.TARGETS[name]['entry']+'.so')
        if binary.exists() and digest(binary) == p['entry_sha256']:
            matches.append(name)
    return sorted(matches)


def steam_audit(profile, tree):
    if profile not in STEAM_PROFILES:
        return False
    config = json.loads(STEAM_PROFILES[profile].read_text())
    for record in config['files']:
        path = Path(tree)/'lib/arm64-v8a'/Path(record['path']).name
        if not path.is_file() or path.stat().st_size != record['size'] or digest(path) != record['sha256']:
            return False
    return True


def inspection_warning(info):
    warning = 'Klepton has no ART/JVM. A successful conversion or installation does not establish gameplay compatibility.'
    limitation = KNOWN_RUNTIME_LIMITATIONS.get(info.get('apk_sha256', ''), '')
    return warning + (' ' + limitation if limitation else '')


def decode_import(snapshot, tree, runner):
    """Decoded APKs are disposable; recreate them from the retained download."""
    if (tree/'AndroidManifest.xml').is_file():
        return
    apktool = shutil.which('apktool')
    if not apktool:
        raise UtilityError('apktool is missing. Run: brew install apktool')
    partial = tree.with_name('decode-pending')
    shutil.rmtree(partial, ignore_errors=True)
    runner.run([apktool, 'd', '-s', '-o', partial, snapshot])
    partial.replace(tree)


def inspect_apk(apk, runner):
    source = Path(apk).expanduser().resolve()
    if not source.is_file():
        raise UtilityError('APK is missing or unreadable. Choose the file again.')
    libraries, expanded = archive_inventory(source)
    if shutil.disk_usage(STORE).free < expanded + source.stat().st_size + 512*1024**2:
        raise UtilityError('Not enough free disk space to snapshot and unpack this APK.')
    runner.start('inspect', 'Reading APK and creating an isolated local copy…')
    imports = STORE/'imports'
    imports.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=imports, prefix='pending-') as tmp:
        snapshot = Path(tmp)/'source.apk'
        shutil.copyfile(source, snapshot)
        fingerprint = digest(snapshot)
        directory = imports/fingerprint
        if not directory.exists():
            directory.mkdir()
            snapshot.replace(directory/'source.apk')
    snapshot = directory/'source.apk'
    if digest(snapshot) != fingerprint:
        raise UtilityError('Stored APK changed. Remove this import cache and inspect the APK again.')
    libraries, _ = archive_inventory(snapshot)
    tree = directory/'tree'
    decode_import(snapshot, tree, runner)
    info = decoded_metadata(tree, libraries)
    info['apk_sha256'] = fingerprint
    # Keep the Makefile as the authority for replaced/injected libraries.
    try:
        info['entry_libraries'] = targets.libs_for(str(tree/'lib/arm64-v8a')).split()
    except SystemExit as error:
        raise UtilityError('No translatable native libraries were discovered. Check make and the ARM64 library set; replaced/injected libraries cannot be entry points.') from error
    matches = match_profiles(info, tree)
    selected = matches[0] if len(matches) == 1 else ''
    # Steam locks also identify original SDKs; full gameplay remains unproven.
    audited = [p for p in STEAM_PROFILES if steam_audit(p, tree)] if info['steam'] else []
    reason = ('Package, version and entry-library fingerprint match an existing profile.' if len(matches) == 1 else
              'Several profiles share these bytes; choose a profile or use Experimental.' if matches else
              'No exact profile match. Native-library detection does not establish compatibility.')
    suggested = []
    for p in source.parent.iterdir():
        if p.is_file() and re.fullmatch(r'(main|patch)\.[0-9]+\.'+re.escape(info['package'])+r'\.obb', p.name):
            suggested.append(str(p))
        elif p.is_dir() and p.name == 'obb':
            suggested.append(str(p))
    result = dict(info, import_id=fingerprint, apk_path=str(snapshot), host_tree=str(tree),
                  source_path=str(source), profile=selected, matches=matches, reason=reason,
                  steam_audited_profiles=audited, suggested_data=sorted(suggested),
                  data_requirements={n: data_requirements(info, n) for n in ['', *targets.TARGETS]},
                  suggested_gles_version='3.1' if fingerprint in KNOWN_RUNTIME_LIMITATIONS else '',
                  transfer_bytes=snapshot.stat().st_size + sum(p.stat().st_size for p in (tree/'assets').rglob('*') if p.is_file()),
                  warning=inspection_warning(info))
    write_json(directory/'inspection.json', result)
    return result


def load_inspection(import_id):
    if not re.fullmatch(r'[0-9a-f]{64}', import_id):
        raise UtilityError('Invalid import identifier. Choose an APK again.')
    directory = STORE/'imports'/import_id
    info = json.loads((directory/'inspection.json').read_text())
    if digest(directory/'source.apk') != import_id:
        raise UtilityError('Imported APK changed. Inspect the original APK again.')
    return info


def resolve_steam(mode, info, profile):
    if mode not in ('auto', 'none', 'runtime', 'offline'):
        raise UtilityError('Unknown Steam mode.')
    audited = steam_audit(profile, info['host_tree'])
    if mode == 'auto':
        mode = 'runtime' if audited else 'offline' if info['steam'] else 'none'
    if mode == 'runtime' and not audited:
        raise UtilityError('Steam runtime requires an audited profile and matching imported SDK binaries. Use Offline diagnostics or choose the matching APK.')
    if mode == 'none' and info['steam']:
        raise UtilityError('This APK contains Steam libraries. Choose Auto, Steam runtime, or Offline diagnostics.')
    return mode


def data_requirements(info, profile):
    package, version = info['package'], info['version_code']
    files = []
    if profile and profile.startswith('roborecall-'):
        files = [f'main.{version}.{package}.obb']
    elif profile == 'beatsaber' and version == '1716':
        files = ['main.1716.com.beatgames.beatsaber.obb']
    elif profile == 'bonelab':
        files = [f'{prefix}.2974.com.StressLevelZero.BONELAB.obb' for prefix in ('main', 'patch')]
    elif profile == 're4':
        files = [f'{prefix}.203.com.Armature.VR4.obb' for prefix in ('main', 'patch')]
    elif profile in RETAIL_PATHS:
        files = ['assets0.pk3', 'assets1.pk3']
    loose = profile in ('walkabout-57013', 'hl1', 'hl2', 'cs1', 'portal', 'vicecity')
    return dict(required=bool(files or loose or info.get('obb_required')), files=files, loose=loose,
                description='Complete game-data folder, including loose bundles' if loose else
                            'Matching external OBB data' if info.get('obb_required') and not files else '')


def validate_request(request, info, job):
    if request.get('schema_version') != 1:
        raise UtilityError('Unsupported request version.')
    gles_version = request.get('gles_version', '')
    if gles_version not in ('', '3.0', '3.1'):
        raise UtilityError('GLES compatibility must be the target default, GLES 3.0 or GLES 3.1.')
    profile = request.get('profile') or None
    if profile and profile not in targets.TARGETS:
        raise UtilityError('Unknown compatibility profile.')
    p = targets.TARGETS.get(profile, {})
    kind = request.get('kind') or p.get('kind') or info['kind']
    if gles_version and kind != 'unity':
        raise UtilityError('The GLES compatibility option is supported for Unity imports only.')
    entry = request.get('entry') or p.get('entry') or info['entry']
    if kind not in KINDS or entry not in info.get('entry_libraries', info['libraries']):
        raise UtilityError('Choose a supported boot type and an entry library present in the imported APK.')
    if not profile and kind not in ('unity', 'ue4', 'native'):
        raise UtilityError('This boot type requires an existing game profile.')
    name = profile or ('import-'+hashlib.sha256(info['package'].encode()).hexdigest()[:16])
    bundle = request.get('bundle') or targets.bundle_for(name)
    if not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', bundle):
        raise UtilityError('Bundle identifier must be dot-separated letters, digits, and hyphens.')
    if not re.fullmatch(r'[A-Z0-9]{10}', request.get('team', '')):
        raise UtilityError('Select a valid Apple development team.')
    if not request.get('device'):
        raise UtilityError('Select a paired Vision Pro.')
    steam = resolve_steam(request.get('steam_mode', 'auto'), info, profile)
    steam_channel = steam_updates.channel_name(request.get('steam_channel', 'stable'))
    adapters = bool(p.get('guest_prepare'))
    if adapters:
        # Walkabout's instruction adapter is separately hash-pinned from its SDK.
        lock = json.loads((ROOT/'games/walkabout/tools/walkabout_login.lock.json').read_text())
        binary = Path(info['host_tree'])/'lib/arm64-v8a/libil2cpp.so'
        adapters = binary.exists() and digest(binary) == lock['binary']['sha256']
    requirements = data_requirements(info, profile)
    data = request.get('data', [])
    if not isinstance(data, list) or any(not isinstance(v, str) for v in data):
        raise UtilityError('Game data must be a list of file or folder paths.')
    paths = [Path(v).expanduser().resolve() for v in data]
    for path in paths:
        if not path.exists() or (not path.is_dir() and path.suffix.lower() != '.obb'):
            raise UtilityError(f'Missing or unsupported game data: {path}. Select OBB files or a data folder.')
        # Symlinks inside folders must be materialized locally before transfer.
        if path.is_dir() and any(x.is_symlink() for x in path.rglob('*')):
            raise UtilityError(f'{path} contains symbolic links. Supply a folder of real files.')
    if requirements['required'] and not paths:
        raise UtilityError('This APK/profile requires external game data. Add its matching OBB files or data folder.')
    supplied = {p.name for path in paths for p in ([path] if path.is_file() else path.rglob('*')) if p.is_file()}
    missing = [name for name in requirements['files'] if name not in supplied]
    if missing:
        raise UtilityError('Missing required game data: '+', '.join(missing))
    if requirements['loose'] and not any(p.is_dir() and any(f.is_file() and f.suffix != '.obb' for f in p.rglob('*')) for p in paths):
        raise UtilityError('This profile needs the complete data folder, including its loose game bundles.')
    folders = {path for path in paths if path.is_dir()}
    suggested_folders = {Path(v).resolve() for v in info.get('suggested_data', []) if Path(v).is_dir()}
    inferred_data_path = info.get('suggested_data_path', '') if not profile and folders and folders <= suggested_folders else ''
    if not profile and folders and not request.get('data_path') and not inferred_data_path:
        raise UtilityError('Choose where this folder’s contents belong inside the game’s Android storage. '
                           'Set Advanced → Data folder destination (for example, obb). Do not enter a folder path on your Mac.')
    obb_path = p.get('obb', 'Android/obb/'+info['package'] if kind == 'ue4' else 'obb')
    data_path = request.get('data_path') or RETAIL_PATHS.get(profile) or p.get('stage_files') or inferred_data_path or obb_path
    if (PurePosixPath(data_path).is_absolute() or '..' in PurePosixPath(data_path).parts or
            not re.fullmatch(r'[A-Za-z0-9_./-]+', data_path)):
        raise UtilityError('Game-data destination must be a safe relative Android storage path.')
    m = dict(schema_version=1, target=name, profile=profile, host_apk=info['apk_path'],
             host_tree=info['host_tree'], kind=kind, entry=entry, bundle=bundle,
             product='KleptonImport'+job.name.replace('-', ''), framework_key='import-'+job.name,
             display=info['name'], steam_mode=steam, steam_channel=steam_channel,
             adapters=adapters, import_id=info['import_id'],
             gles_version=gles_version,
             data=[str(p) for p in paths], data_path=data_path, obb_path=obb_path,
             team=request['team'], device=request['device'])
    return m


def safe_relative(value):
    p = PurePosixPath(value)
    if p.is_absolute() or '..' in p.parts or not p.parts:
        raise UtilityError('CoreDevice returned an unsafe file path; update Xcode before retrying.')
    return str(p)


class Device:
    def __init__(self, runner, identifier, bundle):
        self.runner, self.identifier, self.bundle = runner, identifier, bundle

    @property
    def domain(self):
        return ['--device', self.identifier, '--domain-type', 'appDataContainer', '--domain-identifier', self.bundle]

    def app(self):
        result = self.runner.dev_json(['device', 'info', 'apps', '--device', self.identifier,
                                      '--include-container-paths', '--bundle-id', self.bundle])
        apps = result.get('apps')
        if not isinstance(apps, list):
            raise UtilityError('Unrecognized CoreDevice app inventory; installation stopped before changing the device.')
        return next((a for a in apps if a.get('bundleIdentifier') == self.bundle), None)

    def files(self):
        result = self.runner.dev_json(['device', 'info', 'files', *self.domain, '--recurse'])
        records = result.get('files')
        if not isinstance(records, list):
            raise UtilityError('Unrecognized CoreDevice file inventory; installation stopped to protect saved data.')
        files = []
        self.file_sizes = {}
        for record in records:
            path = record.get('relativePath')
            if not isinstance(path, str):
                raise UtilityError('CoreDevice file inventory has no relativePath; installation stopped to protect saved data.')
            kind = record.get('fileType')
            resources = record.get('resources', {})
            if resources.get('isSymbolicLink'):
                raise UtilityError('Existing app data contains a symbolic link; close the app and back it up in Xcode before updating.')
            directory = resources.get('isDirectory', record.get('isDirectory'))
            if directory is None and kind in ('directory', 'regular', 'file', 'Directory', 'Regular'):
                directory = kind.lower() == 'directory'
            if directory is None:
                raise UtilityError('CoreDevice file inventory has an unknown file type; installation stopped to protect saved data.')
            if not directory:
                files.append(safe_relative(path))
                self.file_sizes[path] = record.get('metadata', {}).get('size', 0)
        return files

    def ensure_closed(self, app):
        url = app.get('url')
        if not url:
            raise UtilityError('CoreDevice did not return the installed app location; cannot safely back up a running app.')
        prefix = unquote(urlparse(url).path).rstrip('/')+'/'
        result = self.runner.dev_json(['device', 'info', 'processes', '--device', self.identifier])
        processes = result.get('runningProcesses')
        if not isinstance(processes, list):
            raise UtilityError('Cannot check whether the existing app is running. Close it and refresh Xcode before retrying.')
        if any(unquote(urlparse(p.get('executable', '')).path).startswith(prefix) for p in processes):
            raise UtilityError('Close the existing game on Vision Pro before updating it, then retry. Saved data must be backed up while the app is stopped.')

    def copy(self, direction, source, destination):
        self.runner.run(['xcrun', 'devicectl', 'device', 'copy', direction, *self.domain,
                         '--source', source, '--destination', destination], timeout=3600)

    def backup(self, job, manifest):
        app = self.app()
        if app is None:
            return []
        self.ensure_closed(app)
        self.runner.start('backup', 'Backing up existing writable data before installation…')
        files = self.files()
        prior = STORE/'receipts'/deployment_key(self.identifier, self.bundle)
        managed = set()
        if prior.exists():
            managed = set(json.loads(prior.read_text()).get('immutable_paths', []))
        # For legacy installs, skip only files identifiable as immutable APK,
        # extracted assets, or OBBs. Unknown files are always backed up.
        tree = manifest['target']
        def immutable(path):
            return (path in managed or path == f'Documents/{tree}.apk' or
                    path.startswith(f'Documents/{tree}/assets/') or
                    (targets.TARGETS.get(manifest.get('profile'), {}).get('obb_raw') == '1' and
                     path.startswith('Documents/android-files/'+manifest.get('obb_path', 'obb')+'/')) or
                    (path.startswith('Documents/android-files/') and path.endswith('.obb')))
        selected = [p for p in files if p.startswith(('Documents/', 'Library/')) and not p.startswith('Library/Caches/')
                    and not immutable(p)]
        if sum(self.file_sizes.get(p, 0) for p in selected) + 512*1024**2 > shutil.disk_usage(job).free:
            raise UtilityError('Not enough free Mac disk space to back up existing game data. Free space before updating.')
        backup = job/'backup'
        roots = backup_groups(files, selected)
        for index, path in enumerate(roots):
            self.runner.check_cancelled()
            local = backup/path
            local.parent.mkdir(parents=True, exist_ok=True)
            self.copy('from', path, str(local))
            emit('backup', 'running', f'Backed up {index+1} of {len(roots)} writable groups.')
        if any(not (backup/p).is_file() for p in selected):
            raise UtilityError('Backup did not produce every expected file; installation stopped.')
        record = [dict(path=p, sha256=digest(backup/p)) for p in selected]
        write_json(job/'backup.json', dict(schema_version=1, device=self.identifier, bundle=self.bundle, files=record))
        self.ensure_closed(app)
        return record

    def restore(self, job, records):
        if not records:
            return
        self.runner.start('restore', 'Restoring the saved writable data into the installed app…')
        for record in records:
            safe_relative(record['path'])
            if not record['path'].startswith(('Documents/', 'Library/')):
                raise UtilityError('Backup contains a path outside writable game data; recovery stopped.')
            path = job/'backup'/record['path']
            if digest(path) != record['sha256']:
                raise UtilityError('A backup file changed. Restore stopped; original backups were retained.')
        for directory in ('Documents', 'Library'):
            path = job/'backup'/directory
            if path.is_dir():
                self.copy('to', str(path), directory)


def backup_groups(files, selected):
    """Coalesce safe subtrees to avoid one expensive CoreDevice call per save."""
    all_files, wanted = set(files), set(selected)
    roots = []
    for path in sorted(wanted):
        if any(path == root or path.startswith(root+'/') for root in roots):
            continue
        parts = PurePosixPath(path).parts
        root = path
        for depth in range(1, len(parts)):
            parent = '/'.join(parts[:depth])
            descendants = {p for p in all_files if p.startswith(parent+'/')}
            if descendants and descendants <= wanted:
                root = parent
                break
        roots.append(root)
    return roots


def environment(manifest_path, manifest):
    # Finder-launched apps have a minimal PATH. No arbitrary inherited KL_ knobs
    # should change the meaning of a form submitted by the user.
    env = {k:v for k,v in os.environ.items() if not k.startswith(('KL_', 'KLEPTON_', 'KLT_', 'MTL_', 'METAL_'))}
    env.update(KLEPTON_BUILD_MANIFEST=str(manifest_path), KLEPTON_TARGET=manifest['target'],
               KLEPTON_BUNDLE_ID=manifest['bundle'], KLEPTON_TEAM=manifest['team'],
               KLEPTON_STEAM_LOCAL='1' if manifest['steam_mode'] == 'runtime' else '0',
               KLEPTON_STEAM_CHANNEL=manifest.get('steam_channel', 'stable'),
               XROS_PLATFORMS='xros', PYTHONUNBUFFERED='1')
    return env


def prepare_payload(manifest, job, backed_up):
    """Materialize only explicitly supplied data; never upload saves over backups."""
    root = job/'payload'
    obb = root/'obb'
    loose = root/'loose'
    destinations = {}
    protected = {r['path'] for r in backed_up}
    for supplied in manifest['data']:
        path = Path(supplied)
        sources = [path] if path.is_file() else sorted(p for p in path.rglob('*') if p.is_file())
        for source in sources:
            relative = source.name if path.is_file() else source.relative_to(path).as_posix()
            is_obb = source.suffix.lower() == '.obb'
            dest_rel = manifest['obb_path']+'/'+source.name if is_obb else manifest['data_path']+'/'+relative
            remote = 'Documents/android-files/'+safe_relative(dest_rel)
            if remote in protected and not is_obb:
                continue  # The existing save always wins over selected seed data.
            if remote in destinations:
                raise UtilityError(f'Two supplied data files map to {dest_rel}; remove the duplicate selection.')
            local = (obb/source.name) if is_obb else (loose/relative)
            local.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, local)
            destinations[remote] = local
    return destinations


def deployment_key(device, bundle):
    return hashlib.sha256((device+'\0'+bundle).encode()).hexdigest()


def recover_job(job, runner):
    job = Path(job).resolve()
    if job.parent != (STORE/'jobs').resolve():
        raise UtilityError('Choose an installer job from this checkout’s recovery message.')
    m = json.loads((job/'manifest.json').read_text())
    pointer = STORE/'pending'/deployment_key(m['device'], m['bundle'])
    if not pointer.exists() or json.loads(pointer.read_text())['job'] != str(job):
        raise UtilityError('This job has no pending saved-data recovery.')
    records = json.loads((job/'backup.json').read_text())['files']
    runner.recovery_job = str(job)
    device = Device(runner, m['device'], m['bundle'])
    # Keep any current data too, so explicit recovery is reversible.
    if device.app() is None:
        raise UtilityError('The app is not installed. Install this job’s signed app with Xcode, then restore saved data again.')
    device.backup(runner.directory, m)
    device.restore(job, records)
    pointer.unlink()
    runner.recovery_job = ''
    return dict(job_path=str(runner.directory), log_path=str(runner.log_path))


def diagnose_logs(text):
    """Report observed failures, never infer playability from a quiet log."""
    missing = re.findall(r"fatal: called unresolved import '([^'\n]+)'", text)
    if missing:
        names = ', '.join(dict.fromkeys(missing))
        return dict(category='runtime', message=f'Klepton is missing a runtime function: {names}. '
                    'This needs a Klepton development fix; changing the boot profile or patching with ovrport will not supply it. '
                    'Attach the collected logs and report.json to a development report.')
    jni = re.findall(r'\[jni\] UNIMPLEMENTED[^\n]*', text)
    methods = re.findall(r'\[jni\] no host implementation for ([^\n]+)', text)
    if jni or methods:
        detail = ': ' + methods[-1][:300] if methods else ''
        return dict(category='runtime', message='The app reached an unsupported runtime/JNI call'+detail+'. '
                    'Attach the collected logs and report.json for a Klepton development fix.')
    if 'WebViewUnavailableException' in text or '[jni] Vuplex browser unavailable:' in text:
        return dict(category='runtime', message='The app reports that its Vuplex WebView browser is unavailable. '
                    'Klepton needs a browser renderer, texture and JavaScript bridge for this feature. '
                    'Changing boot settings, adding OBBs or running ovrport will not implement that bridge. '
                    'Attach the collected logs and report.json for development; any later native fault also needs investigation.')
    if '[webview] navigation failed' in text or '[webview] WebKit content process terminated' in text:
        return dict(category='network', message='The embedded browser could not load its page or its content process stopped. '
                    'Check connectivity and the page address, then retry. TLS/certificate errors remain enforced. '
                    'If it persists, attach the collected logs and report.json for investigation.')
    if '[webview] no host window' in text or '[webview] snapshot failed:' in text:
        return dict(category='runtime', message='The embedded browser could not create or render its page texture. '
                    'Attach the collected logs and report.json for a Klepton development fix.')
    if re.search(r'(?:Unable to open archive file|Failed to read data for the AssetBundle|Unable to load.*(?:globalgamemanagers|data\.unity3d))', text, re.I):
        return dict(category='data', message='The log reports missing or unreadable game content. '
                    'Check matching OBBs, the complete data folder and Advanced → Data folder destination, then rebuild. '
                    'If those are correct, attach these logs for investigation.')
    if re.search(r'(?:SIGSEGV|SIGBUS|SIGABRT|EXC_BAD_ACCESS|\[fault\]|\[klepton\] fault:|fatal:)', text):
        return dict(category='crash', message='The log records a runtime fault. Attach the collected boot/crash logs '
                    'and report.json for development; the log alone does not establish which setting would fix it.')
    if 'HTTP error: Input error. Call GetErrorList()' in text:
        return dict(category='account', message='The app reports an HTTP input-validation error. '
                    'Check that the login fields accepted your text and read any details in the app. '
                    'This message alone does not identify the rejected field or establish that the password is wrong. '
                    'If the form is filled correctly, collect logs for development to investigate input handling '
                    'and compatibility with the service. Do not include passwords or access tokens.')
    if '[plat] account service unavailable:' in text or 'Meta account services are unavailable on this host.' in text:
        return dict(category='account', message='The app requested a signed-in Meta/Oculus account, '
                    'but Klepton has no authenticated Meta account provider. This is a missing platform integration, '
                    'not evidence of an internet outage or an incorrect password. '
                    'If the app offers its own email/password login, try that route. '
                    'Automatic Oculus login needs genuine app-scoped identity and proof from Meta. '
                    'Changing GLES settings, adding a WebView or running ovrport will not provide those credentials. '
                    'Attach the collected logs and report.json for development; do not include passwords or access tokens.')
    return dict(category='unknown', message='No supported failure signature was found. This does not verify that the app runs. '
                'Reproduce the failure, collect logs again, and include what you saw. '
                'If the app closes before writing logs, obtain its crash report in Xcode → Window → Devices and Simulators → View Device Logs.')


def diagnostic_text(path):
    # Bound analysis memory even when a verbose guest produces a huge log.
    with Path(path).open('rb') as stream:
        stream.seek(0, os.SEEK_END)
        stream.seek(max(0, stream.tell() - 2*1024*1024))
        return stream.read(2*1024*1024).decode('utf-8', errors='replace')


def collect_diagnostics(job, runner):
    job = Path(job).expanduser().resolve()
    manifest = json.loads((job/'manifest.json').read_text())
    bundle, identifier = manifest['bundle'], manifest['device']
    if not isinstance(bundle, str) or not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', bundle):
        raise UtilityError('The install job has an invalid bundle identifier.')
    if not isinstance(identifier, str) or not identifier:
        raise UtilityError('The install job has no device identifier.')
    runner.start('diagnose', 'Collecting the last installed app’s headset logs. Keep Vision Pro unlocked…')
    device = Device(runner, identifier, bundle)
    if device.app() is None:
        raise UtilityError('This job’s app is not installed on its Vision Pro. Choose the job for the app that crashed.')
    output = runner.directory/'headset-logs'
    output.mkdir(mode=0o700)
    collected, unavailable = [], []
    # Only diagnostic files: never copy the game’s Documents tree or saved sessions.
    for name in ('klepton-boot.log', 'klepton-crash.log', 'klepton-boot.log.previous', 'klepton-crash.log.previous'):
        try:
            runner.run(['xcrun', 'devicectl', 'device', 'copy', 'from', *device.domain,
                        '--source', 'Documents/'+name, '--destination', output/name,
                        '--timeout', '15'], timeout=20)
            if not (output/name).is_file():
                raise UtilityError('CoreDevice did not return the requested log.')
            collected.append(name)
        except (UtilityError, OSError):
            runner.check_cancelled()
            (output/name).unlink(missing_ok=True)
            unavailable.append(name)
    # Prefer the current run. Previous logs are retained for humans, never mixed
    # into a current diagnosis where an already-fixed failure could mislead it.
    current = [name for name in collected if not name.endswith('.previous')]
    text = '\n'.join(diagnostic_text(output/name) for name in current)
    diagnosis = diagnose_logs(text)
    if not current:
        diagnosis['message'] = ('No current headset logs could be copied. Check the connection, unlock Vision Pro and retry after reproducing the failure. '
                                'For an app that exits before logging, use Xcode → Window → Devices and Simulators → View Device Logs.')
    report = dict(schema_version=1, collected_at=time.time(),
                  app={k:manifest.get(k) for k in ('display', 'bundle', 'target', 'import_id', 'profile', 'kind', 'entry', 'steam_mode', 'gles_version')},
                  collected=collected, unavailable=unavailable, diagnosis=diagnosis)
    write_json(output/'report.json', report)
    return dict(diagnostics_path=str(output), log_path=str(runner.log_path),
                job_path=str(runner.directory), **diagnosis)


def cleanup_installed_job(job):
    """Keep install evidence and downloads; discard completed jobs' working files."""
    job = Path(job).resolve()
    if job.parent != (STORE/'jobs').resolve():
        return []
    if not (job/'result.json').is_file() or not (job/'transfer.json').is_file():
        return []
    # Never prune an interrupted install's only recovery copy. Unreadable
    # pending records also stop cleanup, rather than guessing their ownership.
    for pointer in (STORE/'pending').glob('*'):
        if Path(json.loads(pointer.read_text())['job']).resolve() == job:
            return []
    manifest = json.loads((job/'manifest.json').read_text())
    paths = [job/name for name in ('DerivedData', 'payload', 'backup', 'backup.json',
                                  'post-install-saved-data-readback')]
    import_id = manifest.get('import_id', '')
    if re.fullmatch(r'[0-9a-f]{64}', import_id):
        paths.append(STORE/'imports'/import_id/'tree')
    removed = []
    for path in paths:
        if path.is_symlink():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()
        else:
            continue
        removed.append(str(path))
    return removed


def run_job(request, runner):
    info = load_inspection(request.get('import_id', ''))
    decode_import(Path(info['apk_path']), Path(info['host_tree']), runner)
    runner.start('validate', 'Validating profile, signing, prerequisites, and game data…')
    m = validate_request(request, info, runner.directory)
    if m['steam_mode'] == 'runtime':
        _, selected = steam_updates.resolve(m['steam_channel'])
        m['steam_release'] = {key:selected.get(key) for key in ('version', 'build', 'source', 'sha256')}
    key = deployment_key(m['device'], m['bundle'])
    pending = STORE/'pending'/key
    if pending.exists():
        runner.recovery_job = json.loads(pending.read_text())['job']
        raise UtilityError('A previous update stopped before completion. Restore its saved-data backup before another install; '
                           'use Restore Saved Data in the installer. Backup job: '+runner.recovery_job)
    readiness = doctor()
    needs_vulkan = info.get('vulkan_required') or m['profile'] in ('eleven', 'walkabout-57013', 'bonelab', 'openbrush', 're4')
    failures = [c['name']+': '+c['fix'] for c in readiness['checks'] if not c['ready'] and
                (c.get('required', True) or (c['name'] == 'MoltenVK' and needs_vulkan))]
    if failures:
        raise UtilityError('Setup required:\n'+'\n'.join(failures))
    if m['team'] not in [t['id'] for t in readiness['teams']]:
        raise UtilityError('The selected development team has no valid signing identity in this keychain.')
    if m['device'] not in [d['id'] for d in readiness['devices'] if d['paired']]:
        raise UtilityError('Selected Vision Pro is unavailable. Pair it in Xcode, unlock it, then refresh.')
    manifest_path = runner.directory/'manifest.json'
    write_json(manifest_path, m)
    env = environment(manifest_path, m)
    if m['steam_mode'] == 'runtime':
        # Fail before lengthy compilation if pinned client/UI inputs are absent.
        runner.run([sys.executable, '-c',
            'import sys;sys.path.insert(0,"steam/visionos");import mksteam;mksteam.inputs(sys.argv[1])', m['target']], env)
    device = Device(runner, m['device'], m['bundle'])
    # Check device access now, so a locked device does not waste a long build.
    device.app()
    runner.start('dependencies', 'Building Klepton from the dependencies already on this Mac…')
    runner.run(['make', 'build/klepton-ld', 'xros', 'XROS_PLATFORMS=xros'], env)
    runner.run(['bash', 'visionos/mkangle.sh'], env)
    runner.run(['bash', 'visionos/mkmvk.sh'], env)
    runner.start('translate', 'Translating imported ARM64 native libraries…')
    runner.run(['bash', 'visionos/mkguest.sh'], env)
    if m['steam_mode'] == 'runtime':
        runner.start('steam', 'Preparing the audited local Steam runtime…')
        runner.run([sys.executable, 'steam/visionos/mksteam.py', '--target', m['target'], '--platform', 'visionos'], env)
    runner.start('project', 'Generating the signed app project…')
    runner.run([sys.executable, 'visionos/gen_xcodeproj.py'], env)
    runner.start('build', 'Building and signing the visionOS app…')
    derived = runner.directory/'DerivedData'
    runner.run(['xcodebuild', '-project', f"visionos/{m['product']}.xcodeproj", '-scheme', m['product'],
                '-configuration', 'Debug', '-destination', 'generic/platform=visionOS',
                '-derivedDataPath', derived, '-allowProvisioningUpdates', 'build'], env)
    app = derived/'Build/Products/Debug-xros'/(m['product']+'.app')
    if not (app/'Info.plist').is_file():
        raise UtilityError('Build finished without producing this job’s app. No existing app was installed.')
    runner.run(['codesign', '--verify', '--deep', '--strict', app], env)
    records = device.backup(runner.directory, m)
    # Data collisions must fail before any installation changes the device.
    payload = prepare_payload(m, runner.directory, records)
    receipt_path = STORE/'receipts'/key
    previous = json.loads(receipt_path.read_text()) if receipt_path.exists() else None
    payload_records = [dict(path=p, sha256=digest(f)) for p,f in payload.items()]
    runner.check_cancelled()
    # Invalidate before the first mutation; interruption cannot leave a success stamp.
    receipt_path.unlink(missing_ok=True)
    if records:
        write_json(pending, dict(job=str(runner.directory.resolve())))
        runner.recovery_job = str(runner.directory.resolve())
    runner.start('install', 'Installing the signed app on Vision Pro…')
    runner.run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', m['device'], app], timeout=600)
    device.restore(runner.directory, records)
    runner.start('stage', 'Transferring the APK, assets, and supplied game data…')
    installed_app = device.app()
    container = installed_app.get('dataContainerPath', '') if installed_app else ''
    reusable = bool(previous and container and previous.get('container') == container and
                    previous.get('import_id') == m['import_id'] and previous.get('data') == payload_records)
    if reusable:
        try:
            proof = runner.directory/'previous-transfer.json'
            device.copy('from', 'Documents/klepton-installer-transfer.json', str(proof))
            reusable = json.loads(proof.read_text()) == previous
        except (UtilityError, OSError, ValueError):
            runner.check_cancelled()
            reusable = False
    # Use the shared staging implementation for APK, assets, metadata and Qt
    # inspection files. Data and saves use the checked paths above.
    stage_env = dict(env, KL_SKIP_OBB='1', KL_SKIP_FILES='1', KL_SKIP_RETAIL='1')
    if reusable:
        runner.run(['bash', 'visionos/stage_assets.sh', '--meta', m['device']], stage_env)
        emit('stage', 'running', 'Unchanged payload already staged in this app container; refreshed metadata.')
    else:
        runner.run(['bash', 'visionos/stage_assets.sh', m['device']], stage_env)
        for index, (remote, local) in enumerate(payload.items()):
            device.copy('to', str(local), remote)
            emit('stage', 'running', f'Transferred {index+1} of {len(payload)} game-data files.')
    # A remote receipt proves that staging reached its end in this container.
    receipt = dict(schema_version=1, device=m['device'], bundle=m['bundle'], import_id=m['import_id'],
                   job=str(runner.directory), container=container, installed_at=time.time(),
                   immutable_paths=[p for p in payload if p.endswith('.obb')],
                   data=payload_records)
    local_receipt = runner.directory/'transfer.json'
    write_json(local_receipt, receipt)
    device.copy('to', str(local_receipt), 'Documents/klepton-installer-transfer.json')
    runner.check_cancelled()
    write_json(receipt_path, receipt)
    pending.unlink(missing_ok=True)
    runner.recovery_job = ''
    result = dict(app_path=str(app), log_path=str(runner.log_path), job_path=str(runner.directory),
                  bundle=m['bundle'], device=m['device'], manifest_path=str(manifest_path))
    write_json(runner.directory/'result.json', result)
    try:
        removed = cleanup_installed_job(runner.directory)
        if removed:
            result['cleaned_paths'] = removed
    except (OSError, ValueError, KeyError) as error:
        # Installation already succeeded. A cache-cleanup failure must not
        # report it as a failed install or interfere with saved-data recovery.
        emit('cleanup', 'warning', 'Installed successfully, but temporary files could not all be removed: '+str(error))
    if not app.exists():
        result['app_path'] = ''
    write_json(runner.directory/'result.json', result)
    return result


def update_steam(channel, runner):
    """Prepare isolated acquisition tools only when Update is explicitly clicked."""
    steam_updates.channel_name(channel)
    root = steam_updates.STORE
    python = root/'venv/bin/python'
    requirements = ROOT/'steam/tools/steam_updates.requirements.txt'
    stamp = root/'requirements.sha256'
    runner.start('steam-update', 'Preparing Steam update tools…')
    if not python.is_file():
        runner.run([sys.executable, '-m', 'venv', root/'venv'])
    if not stamp.is_file() or stamp.read_text() != digest(requirements):
        runner.run([python, '-m', 'pip', 'install', '--disable-pip-version-check', '-r', requirements], timeout=600)
        stamp.write_text(digest(requirements))
    desync = root/'tools/desync'
    if not desync.is_file():
        if not shutil.which('go'):
            raise UtilityError('Steam updates need Go to build the pinned desync downloader. Run brew install go, then click Update again.')
        env = dict(os.environ, GOBIN=str(desync.parent))
        runner.run(['go', 'install', 'github.com/folbricht/desync/cmd/desync@' + steam_updates.DESYNC_VERSION], env, timeout=600)
    runner.start('steam-update', 'Downloading and verifying Steam ' + channel + ' client and login files. This may take several minutes…')
    runner.run([python, ROOT/'steam/tools/steam_updates.py', '--channel', channel, '--desync', desync])
    return steam_updates.status()


def steam_game_data(info, folder, receipt):
    """Remember the downloaded data layout independently of compatibility pins."""
    data, unmapped = steam_library.data_candidates(folder, receipt)
    info['source_path'] = str(folder.joinpath(*steam_library.safe_path(receipt['executable']).parts))
    info['suggested_data'] = data
    info['suggested_data_path'] = (
        ('Android/obb/'+info['package'] if info['kind'] == 'ue4' else 'obb')
        if data and info.get('kind') in ('unity', 'native', 'ue4') else '')
    write_json(STORE/'imports'/info['import_id']/'inspection.json', info)
    return data, unmapped


def library_job(action, runner):
    runner.start('steam-library', 'Preparing Steam library…')
    # Read only from the private input pipe; never save or log this request.
    try:
        request = json.loads(sys.stdin.readline())
    except ValueError:
        raise UtilityError('Invalid Steam library request.') from None
    cache = steam_library.LibraryStore(STORE, digest, write_json)
    if action == 'cached':
        emit('steam-library', 'result', **cache.cached(request.get('steam_id', '')))
        return
    if action == 'use':
        steam_id = steam_library.account_id(request.get('steam_id', ''))
        local = cache.local(steam_id, request.get('app_id', ''), request.get('branch', 'public'), request.get('launch_id', ''))
        if not local or not local['valid']:
            emit('steam-library', 'result', 'Downloaded files need repair.', **cache.cached(steam_id))
            raise UtilityError('Downloaded files are missing or damaged. Click Repair to download them again.')
        folder = Path(local['folder'])
        result = inspect_apk(folder.joinpath(*steam_library.safe_path(local['executable']).parts), runner)
        data, unmapped = steam_game_data(result, folder, local)
        emit('steam-game', 'result', **result, downloaded_data=data, unmapped_data=unmapped, download_folder=str(folder))
        return
    helper_request = {k: request[k] for k in ('refresh_token', 'auth_method', 'account_name', 'password',
                                              'app_id', 'branch', 'launch_id') if k in request}
    helper_request['action'] = action
    if action in ('login', 'refresh'):
        if request.get('scope') == 'vr_android' and request.get('steam_id'):
            cached = cache.read_catalog(request['steam_id'])
            helper_request.update(steam_id=request['steam_id'],
                                  app_ids=[g['app_id'] for g in cached['games'] if g['vr'] and g['android_available']],
                                  known_app_ids=cached.get('owned_app_ids', [g['app_id'] for g in cached['games']]))
        if action == 'refresh' and request.get('steam_id'):
            emit('steam-library', 'result', 'Checking remote Steam versions…', **cache.cached(request['steam_id']))
        result = steam_library.run_helper(ROOT, helper_request, runner, emit)
        emit('steam-library', 'result', 'Steam library refreshed.', **cache.save_catalog(result))
        return
    cache.root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache.root, prefix='pending-') as directory:
        pending = Path(directory)
        helper_request['destination'] = str(pending)
        result = steam_library.run_helper(ROOT, helper_request, runner, emit)
        runner.start('steam-library', 'Verifying and inspecting downloaded Android files…')
        info, receipt, folder, catalog = cache.publish(result, pending, lambda apk: inspect_apk(apk, runner), runner.check_cancelled)
        data, unmapped = steam_game_data(info, folder, receipt)
        emit('steam-library', 'result', 'Steam download verified and saved.', **catalog)
        emit('steam-game', 'result', **info, downloaded_data=data, unmapped_data=unmapped, download_folder=str(folder))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('doctor')
    sub.add_parser('steam-status')
    library = sub.add_parser('steam-library')
    library.add_argument('--action', choices=('login', 'refresh', 'cached', 'download', 'use'), required=True)
    steam_update = sub.add_parser('steam-update')
    steam_update.add_argument('--channel', choices=steam_updates.CHANNELS, required=True)
    inspect = sub.add_parser('inspect')
    inspect.add_argument('--apk', required=True)
    run = sub.add_parser('run')
    run.add_argument('--request', required=True, type=Path)
    recover = sub.add_parser('recover')
    recover.add_argument('--job', required=True, type=Path)
    diagnose = sub.add_parser('diagnose')
    diagnose.add_argument('--job', required=True, type=Path)
    args = parser.parse_args(argv)
    runner = None
    try:
        if args.command == 'doctor':
            emit('doctor', 'result', **doctor())
            return 0
        if args.command == 'steam-status':
            emit('steam-status', 'result', channels=steam_updates.status())
            return 0
        with checkout_lock():
            job = STORE/'jobs'/uuid.uuid4().hex[:12]
            request = json.loads(args.request.read_text()) if args.command == 'run' else {}
            runner = Runner(job, verbose=bool(request.get('verbose', False)))
            runner.install_handlers()
            if args.command == 'steam-library':
                library_job(args.action, runner)
            elif args.command == 'inspect':
                result = inspect_apk(args.apk, runner)
                emit('inspect', 'result', log_path=str(runner.log_path), **result)
            elif args.command == 'recover':
                result = recover_job(args.job, runner)
                emit('recover', 'result', 'Saved data restored. You can retry Build & Install.', **result)
            elif args.command == 'diagnose':
                result = collect_diagnostics(args.job, runner)
                emit('diagnose', 'result', **result)
            elif args.command == 'steam-update':
                result = update_steam(args.channel, runner)
                current = result[args.channel]['current'] or {}
                message = 'Steam ' + args.channel + ' files updated.'
                if current.get('compatible') is False:
                    message += ' This payload needs a Klepton login bridge compatibility update before use.'
                emit('steam-status', 'result', message, channels=result, log_path=str(runner.log_path))
            else:
                result = run_job(request, runner)
                emit('complete', 'result', 'Installed — open it on Vision Pro. If it closes or fails to start, use Collect Headset Logs.', **result)
        return 0
    except (UtilityError, OSError, ValueError, KeyError, ET.ParseError) as error:
        cancelled = isinstance(error, Cancelled)
        emit(runner.stage if runner else 'setup', 'cancelled' if cancelled else 'error', str(error),
             log_path=str(runner.log_path) if runner else '',
             job_path=str(runner.directory) if runner else '',
             recovery_job=runner.recovery_job if runner else '')
        return 130 if cancelled else 1
    finally:
        if runner:
            runner.restore_handlers()


if __name__ == '__main__':
    sys.exit(main())
