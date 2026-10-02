#!/usr/bin/env python3
"""Stage the pinned local Steam runtime for experimental visionOS game hosting."""
import argparse
import hashlib
import json
import plistlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'visionos/build/SteamLocal'
FRAMEWORKS = ROOT/'visionos/Frameworks/SteamLocal'
sys.path.insert(0, str(ROOT/'steam/tools'))
import steam_auth_ui


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    recovery = json.loads((ROOT/'steam/tools/steamframe_recovery.lock.json').read_text())
    client = next(v for v in recovery['artifacts'] if v['path'] == 'androidarm64/libsteamclient.so')
    if client['sha256'] != '4b1318ea53168ecbf74318ef5b330eb0a41a3e2d335c14f3827504bebba4b40d':
        raise ValueError('game-host private ABI requires the audited recovery client')
    original = ROOT/'build/steam-runtime/steamframe-0.3.0/steam'/client['path']
    records = [dict(client, path=str(original.relative_to(ROOT)))]
    records += json.loads((ROOT/'games/walkabout/steam/walkabout_steam_api.lock.json').read_text())['files']
    for record in records:
        path = ROOT/record['path']
        if not path.is_file() or path.stat().st_size != record['size'] or digest(path) != record['sha256']:
            raise ValueError('missing or different pinned Steam input: '+record['path'])
    return original, records


import runpy
HOST_SWIFT = runpy.run_path(str(ROOT/'games/walkabout/steam/host.py'))['HOST_SWIFT']


def stage(platform='visionossim'):
    if platform not in ('visionos', 'visionossim'):
        raise ValueError('unsupported Steam framework platform')
    original, records = inputs()
    recovery = json.loads((ROOT/'steam/tools/steamframe_recovery.lock.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    FRAMEWORKS.mkdir(parents=True, exist_ok=True)
    subprocess.run(['make', 'build/klepton-ld'], cwd=ROOT, check=True)
    artifacts = []
    translations = [('libsteamclient_backend', original), ('libsteamclient', original)]
    translations += [(Path(record['path']).stem, ROOT/record['path']) for record in records[1:]]
    for name, source in translations:
        fwname = name.replace('+', 'x')
        framework = OUT/(fwname+'.framework'); framework.mkdir(exist_ok=True)
        target = framework/fwname
        subprocess.run([str(ROOT/'build/klepton-ld'), str(source), '-o', str(target),
                        '--platform', platform, '--install-name', f'@rpath/{fwname}.framework/{fwname}', '--quiet'], check=True)
        plistlib.dump({'CFBundleExecutable': fwname, 'CFBundleIdentifier':'dev.klepton.steam.'+fwname.replace('_','-'),
            'CFBundleName':fwname, 'CFBundlePackageType':'FMWK', 'CFBundleVersion':'1',
            'CFBundleShortVersionString':'1.0', 'MinimumOSVersion':'26.0',
            'CFBundleSupportedPlatforms':['XROS' if platform == 'visionos' else 'XRSimulator']}, (framework/'Info.plist').open('wb'))
        xc = FRAMEWORKS/(name+'.xcframework')
        # Only replace this tool's disposable output, never another target.
        if xc.exists():
            import shutil
            shutil.rmtree(xc)
        subprocess.run(['xcodebuild','-create-xcframework','-framework',str(framework),'-output',str(xc)], check=True)
        embedded = xc/('xros-arm64' if platform == 'visionos' else 'xros-arm64-simulator')/framework.name/fwname
        if not embedded.is_file(): raise ValueError('unexpected generated Steam XCFramework layout')
        artifacts.append({'path':str(embedded.relative_to(ROOT)), 'sha256':digest(embedded)})
    assets = OUT/'SteamAuthAssets'
    ui = steam_auth_ui.package(ROOT/'build/steam-runtime/steamframe-0.3.0/steam/steamui', assets, interactive=True)
    swift = OUT/'SteamHostLogin.swift'
    swift.write_text(HOST_SWIFT+(ROOT/'steam/tools/steam_auth_ui_login.swift').read_text())
    # The standalone branch is excluded at compile time; resolve its template
    # placeholders as well so the generated Swift always remains well-formed.
    swift.write_text(swift.read_text().replace('@RUN_ID@', 'standalone-branch-unused')
                     .replace('@WALKABOUT_API_ENV@', 'setenv("KL_STEAM_WALKABOUT_API", "1", 1)'))
    artifacts.append({'path':str(swift.relative_to(ROOT)), 'sha256':digest(swift)})
    for path in sorted(p for p in assets.rglob('*') if p.is_file()):
        artifacts.append({'path':str(path.relative_to(ROOT)), 'sha256':digest(path)})
    receipt = {'schema_version':1, 'platform':platform, 'source':'steamframe-recovery',
        'release':recovery['release'], 'recovery_sha256':recovery['sha256'],
        'inputs':records, 'auth_ui_inputs':ui, 'artifacts':artifacts,
        'physical_execution_verified':False, 'game_playfab_verified':False}
    (OUT/'manifest.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


def verify_staged():
    _, records = inputs()
    receipt = json.loads((OUT/'manifest.json').read_text())
    if receipt['platform'] not in ('visionos', 'visionossim') or receipt['inputs'] != records:
        raise ValueError('Steam staging manifest does not match the pinned visionOS inputs')
    for record in receipt['artifacts']:
        path = ROOT/record['path']
        if not path.is_file() or digest(path) != record['sha256']:
            raise ValueError('staged Steam artifact changed: '+record['path'])
    canonical = HOST_SWIFT+(ROOT/'steam/tools/steam_auth_ui_login.swift').read_text()
    canonical = canonical.replace('@RUN_ID@','standalone-branch-unused').replace('@WALKABOUT_API_ENV@','setenv("KL_STEAM_WALKABOUT_API", "1", 1)')
    if (OUT/'SteamHostLogin.swift').read_text() != canonical:
        raise ValueError('Steam login sources changed; rerun steam/visionos/mksteam.py')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--platform', choices=['visionos', 'visionossim'], default='visionossim')
    args = parser.parse_args()
    try:
        verify_staged() if args.verify else stage(args.platform)
        print('Verified local Steam visionOS inputs:', OUT/'manifest.json')
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('Steam staging failed:', error, file=sys.stderr); sys.exit(1)
