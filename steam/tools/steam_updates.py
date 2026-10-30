#!/usr/bin/env python3
"""Steam Frame channels from Valve's atomupd feed, with signed RAUC acquisition.

No Valve code is executed. Published channel receipts point to immutable builds;
an interrupted download never replaces the previously installed channel.
"""
import argparse
import configparser
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import subprocess
import tarfile
import tempfile
import uuid
from urllib.parse import urlparse
from urllib.request import urlopen

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
STORE = REPO / 'build/steam-updates'
CHANNELS = ('stable', 'beta')
META = 'https://steamdeck-atomupd.steamos.cloud/meta/holo/steamos/aarch64/vr'
IMAGES = 'https://steamdeck-images.steamos.cloud'
CA_FINGERPRINT = 'd562fefe251b76eab0e9e999354ad186b8eb2e2fb3450fadbec7f6aa59b602d9'
DEV_CA_FINGERPRINT = '3d4bc22d566c1362aca160c21d6028994c9054eceaa20da37507a44f0ab98993'
DESYNC_VERSION = 'v1.1.4'


def channel_name(channel):
    if channel not in CHANNELS:
        raise ValueError('Unknown Steam channel. Choose stable or beta.')
    return channel


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    # A unique temporary also permits concurrent read-only status requests.
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as f:
        json.dump(value, f, indent=2)
        f.write('\n')
    Path(f.name).replace(path)


def remote_path(value):
    if not isinstance(value, str) or not re.fullmatch(r'vr/[A-Za-z0-9_./-]+', value) or '..' in value.split('/'):
        raise ValueError('Unexpected Valve update path.')
    return value


def download(url, path, limit):
    expected = urlparse(url)
    if expected.scheme != 'https' or expected.hostname not in (
            'steamdeck-atomupd.steamos.cloud', 'steamdeck-images.steamos.cloud'):
        raise ValueError('Expected a Valve SteamOS HTTPS URL.')
    temporary = path.with_suffix(path.suffix + '.part')
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urlopen(url, timeout=20) as response, temporary.open('wb') as out:
            final = urlparse(response.url)
            if final.scheme != 'https' or final.hostname != expected.hostname:
                raise ValueError('Unexpected Valve update redirect.')
            count = 0
            while data := response.read(1024 * 1024):
                count += len(data)
                if count > limit:
                    raise ValueError('Valve update exceeds the download budget.')
                out.write(data)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def candidate(document, channel):
    channel_name(channel)
    candidates = document.get('minor', {}).get('candidates', [])
    matching = []
    for record in candidates:
        image = record.get('image', {})
        if (image.get('arch'), image.get('product'), image.get('release'), image.get('variant'), image.get('branch')) != (
                'aarch64', 'steamos', 'holo', 'vr', channel):
            continue
        if not re.fullmatch(r'\d+(?:\.\d+)+', image.get('version', '')) or not re.fullmatch(r'\d{8}\.\d+', image.get('buildid', '')):
            raise ValueError('Invalid Steam Frame release/build number.')
        path = remote_path(record['update_path'])
        if not path.endswith('.raucb') or path.split('/')[1] != image['buildid']:
            raise ValueError('Valve update path does not match its build.')
        matching.append(dict(channel=channel, version=image['version'], build=image['buildid'],
                             update_path=path, chunks_store_path=remote_path(record['chunks_store_path'])))
    if not matching:
        raise ValueError('Valve did not advertise a Steam Frame ' + channel + ' update.')
    return max(matching, key=lambda r: tuple(map(int, r['build'].split('.'))))


def latest(channel):
    channel_name(channel)
    with tempfile.TemporaryDirectory(prefix='klepton-steam-feed-') as folder:
        path = Path(folder)/'feed.json'
        download(f'{META}/{channel}.json', path, 1024 * 1024)
        return candidate(json.loads(path.read_text()), channel)


def installed(channel):
    channel_name(channel)
    pointer = STORE/'channels'/(channel + '.json')
    if pointer.exists():
        record = json.loads(pointer.read_text())
        build = record.get('build', '')
        if not re.fullmatch(r'\d{8}\.\d+', build):
            raise ValueError('Invalid installed Steam build receipt.')
        generation = record.get('generation', '')
        if not re.fullmatch(r'[a-f0-9]{32}', generation):
            raise ValueError('Invalid installed Steam generation receipt.')
        root = STORE/'builds'/build/generation
        receipt = json.loads((root/'receipt.json').read_text())
        if receipt['build'] != build or receipt['version'] != record['version']:
            raise ValueError('Installed Steam channel receipt does not match its build.')
        return root/'steam', receipt
    if channel == 'stable':
        lock = json.loads((HERE/'steamframe_recovery.lock.json').read_text())
        root = REPO/'build/steam-runtime'/('steamframe-' + lock['release'])/'steam'
        if (root/'androidarm64/libsteamclient.so').is_file():
            ui = json.loads((HERE/'steam_auth_ui.lock.json').read_text())['files']
            files = [next(r for r in lock['artifacts'] if r['path'] == 'androidarm64/libsteamclient.so')]
            files += [dict(r, path='steamui/' + r['path']) for r in ui]
            return root, dict(version=lock['release'], build=lock['build'], source='steamframe-recovery',
                              sha256=lock['sha256'], files=files, compatible=True)
    return None, None


def resolve(channel):
    root, receipt = installed(channel)
    if not receipt:
        raise ValueError('Steam ' + channel + ' is not installed. Click its Update button first.')
    if not receipt.get('compatible'):
        raise ValueError('Steam ' + channel + ' ' + receipt['version'] +
                         ' needs a Klepton login bridge compatibility update. ' + receipt.get('compatibility_error', ''))
    for record in receipt['files']:
        from steam_runtime import archive_path
        path = root/archive_path(record['path'])
        if not path.is_file() or path.stat().st_size != record['size'] or digest(path) != record['sha256']:
            raise ValueError('Missing or changed Steam ' + channel + ' input: ' + record['path'] + '. Click Update to repair it.')
    return root, receipt


def status(remote=True):
    def one(channel):
        result = dict(channel=channel, current=None, remote=None, error='')
        try:
            _, receipt = installed(channel)
            if receipt:
                result['current'] = {key:receipt.get(key) for key in ('version', 'build', 'compatible', 'compatibility_error')}
                if receipt.get('compatible'):
                    try:
                        resolve(channel)
                    except (OSError, ValueError, KeyError) as error:
                        result['current']['compatible'] = False
                        result['current']['compatibility_error'] = str(error)
        except (OSError, ValueError, KeyError) as error:
            result['error'] = str(error)
        if remote:
            try:
                result['remote'] = latest(channel)
            except (OSError, ValueError, KeyError) as error:
                result['error'] = str(error)
        return result
    with ThreadPoolExecutor(max_workers=2) as pool:
        return dict(zip(CHANNELS, pool.map(one, CHANNELS)))


def verify_bundle(bundle, work, channel='stable'):
    """Verify the entire signed squashfs before reading its index or manifest."""
    channel_name(channel)
    ca = HERE/('steamos-dev-images.pem' if channel == 'beta' else 'steamdeck-images.pem')
    fingerprint = DEV_CA_FINGERPRINT if channel == 'beta' else CA_FINGERPRINT
    if hashlib.sha256(ssl.PEM_cert_to_DER_cert(ca.read_text())).hexdigest() != fingerprint:
        raise ValueError('Valve RAUC CA fingerprint mismatch.')
    if channel == 'beta':
        production = HERE/'steamdeck-images.pem'
        if hashlib.sha256(ssl.PEM_cert_to_DER_cert(production.read_text())).hexdigest() != CA_FINGERPRINT:
            raise ValueError('Valve production RAUC CA fingerprint mismatch.')
        combined = work/'trusted-keys.pem'
        combined.write_text(production.read_text() + '\n' + ca.read_text())
        ca = combined
    data = bundle.read_bytes()
    size = int.from_bytes(data[-8:], 'big')
    if size < 32 or size > len(data) - 8:
        raise ValueError('Invalid Valve RAUC signature trailer.')
    payload, signature = work/'bundle.sqfs', work/'bundle.sig'
    payload.write_bytes(data[:-size-8])
    signature.write_bytes(data[-size-8:-8])
    subprocess.run(['openssl', 'cms', '-verify', '-binary', '-inform', 'DER', '-in', str(signature),
                    '-content', str(payload), '-CAfile', str(ca), '-purpose', 'any', '-out', os.devnull], check=True)
    from dissect.squashfs import SquashFS
    with payload.open('rb') as stream:
        fs = SquashFS(stream)
        manifest = configparser.ConfigParser()
        manifest.read_string(fs.get('manifest.raucm').open().read().decode())
        image = manifest['image.rootfs']
        if (manifest['update']['compatible'] != 'steamos-aarch64' or image['filename'] != 'rootfs.img.caibx'
                or not re.fullmatch('[a-f0-9]{64}', image['sha256']) or not 0 < int(image['size']) <= 16 * 1024**3):
            raise ValueError('Unexpected signed Steam Frame root filesystem manifest.')
        (work/'rootfs.img.caibx').write_bytes(fs.get(image['filename']).open().read())
    return dict(sha256=image['sha256'], size=int(image['size']), build=manifest['update']['version'])


def extract_payload(image, work):
    from dissect.btrfs import Btrfs
    from backports.zstd import ZstdFile
    from steam_runtime import archive_path
    from steamframe_recovery import write_member
    archive = work/'steam.tar.zst'
    print('Extracting Steam client and login assets from the verified root filesystem…', flush=True)
    with image.open('rb') as stream:
        fs = Btrfs(stream)
        node = fs.get('/usr/lib/steam/steam.tar.zst')
        if node.size > 2 * 1024**3:
            raise ValueError('Steam payload exceeds extraction budget.')
        with node.open() as source:
            write_member(source, archive, node.size, work)
    destination = work/'steam'
    files, seen, total = [], set(), 0
    with ZstdFile(archive, 'rb') as stream, tarfile.open(fileobj=stream, mode='r|') as tar:
        for member in tar:
            name = str(archive_path(member.name))
            if not (name.startswith('androidarm64/') or name.startswith('steamui/') and name.endswith(('.js', '.html', '.css', '.json'))):
                continue
            if member.isdir():
                continue
            if name in seen or not member.isfile() or member.size > 256 * 1024**2:
                raise ValueError('Invalid or duplicate Steam payload member: ' + name)
            seen.add(name)
            total += member.size
            if total > 1024**3:
                raise ValueError('Steam payload exceeds extraction budget.')
            with tar.extractfile(member) as source:
                record = write_member(source, destination/name, member.size, destination)
            files.append(dict(path=name, **record))
    return files


def client_profile(client_digest):
    profiles = json.loads((HERE/'steam_client_profiles.json').read_text())['profiles']
    if client_digest not in profiles:
        raise ValueError('This libsteamclient.so needs a Klepton private login ABI profile.')
    return profiles[client_digest]


def login_header(client_digest):
    profile = client_profile(client_digest)
    text = '// Generated from the hash-selected Steam client profile: ' + profile['id'] + '\n'
    text += '#include <stddef.h>\n#include <stdint.h>\n'
    text += 'static inline size_t klepton_steam_login_offset(size_t offset) {\n'
    if profile['offsets'] is None:
        text += '    return offset;\n'
    else:
        text += '    switch (offset) {\n'
        for old, new in profile['offsets'].items():
            text += f'    case {int(old, 16):#x}: return {int(new, 16):#x};\n'
        text += '    default: return SIZE_MAX;\n    }\n'
    return text + '}\n#define KL_STEAM_LOGIN_OFFSET(offset) klepton_steam_login_offset(offset)\n'


def ui_records(files):
    return [dict(r, path=r['path'][len('steamui/'):]) for r in files if r['path'].startswith('steamui/')]


def compatibility(root, files):
    """Select a hash-specific ABI profile and check the signed UI input set."""
    import steam_auth_ui
    try:
        client_profile(digest(root/'androidarm64/libsteamclient.so'))
    except ValueError as error:
        return False, str(error)
    with tempfile.TemporaryDirectory(prefix='klepton-steam-ui-') as folder:
        try:
            steam_auth_ui.package(root/'steamui', Path(folder), interactive=True, records=ui_records(files))
        except (OSError, ValueError) as error:
            return False, str(error)
    return True, ''


def update(channel, desync):
    release = latest(channel)
    STORE.mkdir(parents=True, exist_ok=True)
    cache = STORE/'downloads'/release['build']
    cache.mkdir(parents=True, exist_ok=True)
    generation = uuid.uuid4().hex
    destination = STORE/'builds'/release['build']/generation
    # Re-download/re-verify into new storage: Update also repairs changed files.
    with tempfile.TemporaryDirectory(prefix='.staging-', dir=STORE) as folder:
        work = Path(folder)
        print(f"Fetching signed Steam Frame {channel} {release['version']} ({release['build']})…", flush=True)
        bundle = cache/'update.raucb'
        download(IMAGES + '/' + release['update_path'], bundle, 32 * 1024**2)
        image = verify_bundle(bundle, work, channel)
        if image['build'] != release['build']:
            raise ValueError('Signed bundle build differs from the Valve update feed.')
        rootfs = cache/'rootfs.img'
        if not rootfs.is_file() or rootfs.stat().st_size != image['size'] or digest(rootfs) != image['sha256']:
            if shutil.disk_usage(cache).free < image['size'] + 3 * 1024**3:
                raise ValueError('Free at least 13 GiB for the Steam Frame update, then retry.')
            print('Downloading SteamOS chunks (10 GiB expanded); the previous channel remains available…', flush=True)
            subprocess.run([str(desync), 'extract', '-k', '-n', '16', '--error-retry', '5',
                            '-s', IMAGES + '/' + release['update_path'][:-6] + '.castr/',
                            '-s', IMAGES + '/' + release['chunks_store_path'] + '/',
                            str(work/'rootfs.img.caibx'), str(rootfs)], check=True)
        if rootfs.stat().st_size != image['size'] or digest(rootfs) != image['sha256']:
            raise ValueError('Steam Frame root filesystem size/SHA-256 mismatch.')
        files = extract_payload(rootfs, work)
        required = {'androidarm64/libsteamclient.so', 'androidarm64/steamservice.so'}
        if not required.issubset({r['path'] for r in files}):
            raise ValueError('Updated Steam payload is missing required Android ARM64 libraries.')
        from steam_runtime import elf_inventory
        for name in required:
            info = elf_inventory((work/'steam'/name).read_bytes())
            if not info or info['architecture'] != 'aarch64':
                raise ValueError('Updated Steam library is not ARM64: ' + name)
        usable, reason = compatibility(work/'steam', files)
        receipt = dict(release, schema_version=1, source='valve-atomupd', files=files,
                       sha256=digest(bundle), rootfs_sha256=image['sha256'],
                       compatible=usable, compatibility_error=reason)
        write_json(work/'receipt.json', receipt)
        # Publish only after every file and receipt is in place. Previous
        # generations remain intact even if cancelled immediately before this.
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.mkdir()
        (work/'steam').replace(destination/'steam')
        (work/'receipt.json').replace(destination/'receipt.json')
        write_json(STORE/'channels'/(channel + '.json'), dict(generation=generation,
                   **{k:release[k] for k in ('version', 'build')}))
        rootfs.unlink()  # Only the required Steam files survive a completed update.
        print(f"Installed Steam Frame {channel} {release['version']}." + ('' if usable else ' ' + reason), flush=True)
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', required=True, choices=CHANNELS)
    parser.add_argument('--desync', type=Path, required=True)
    args = parser.parse_args()
    update(args.channel, args.desync)
