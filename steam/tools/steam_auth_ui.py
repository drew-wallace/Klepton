"""Package hash-pinned Valve modules for the standalone, anonymous UI probe.

Valve artifacts and generated bootstrap stay in ignored build/. The desktop
shell entry point is suppressed; module bodies and Valve's Webpack loader are
preserved. No Steam authentication protocol is implemented here.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOCK = ROOT / 'steam_auth_ui.lock.json'
ENTRY = 'var bs=ve.O(void 0,[41],()=>ve(2804));bs=ve.O(bs)'


def package(source, destination, interactive=False, records=None):
    if records is None:
        records = json.loads(LOCK.read_text())['files']
        bundle = 'chunk~2dcc5aaf7.js'
    else:
        # The hotfix keeps the same Webpack module IDs/exports but renames the
        # shared bundle to sp.js. Trust its bytes through the signed OS receipt.
        names = {r['path'] for r in records}
        bundle = 'chunk~2dcc5aaf7.js' if 'chunk~2dcc5aaf7.js' in names else 'sp.js'
        needed = ['library.js', bundle, 'chunk~1a96cdf59.js', 'libraries/libraries~00299a408.js']
        indexed = {r['path']:r for r in records}
        if not set(needed).issubset(indexed):
            raise ValueError('Steam payload lacks the required login UI modules.')
        records = [indexed[name] for name in needed]
    checked = {}
    for record in records:
        content = (source / record['path']).read_bytes()
        if len(content) != record['size'] or hashlib.sha256(content).hexdigest() != record['sha256']:
            raise ValueError('Valve auth UI artifact does not match its pin: ' + record['path'])
        checked[record['path']] = content
    main = checked['library.js'].decode('utf-8')
    if main.count(ENTRY) != 1:
        raise ValueError('Unrecognized Valve UI bootstrap')
    # Only expose the existing loader. Do not launch the desktop library shell.
    checked['library.js'] = main.replace(ENTRY, 'globalThis.KleptonValveRequire=ve').encode('utf-8')
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in checked.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    (destination/'probe.js').write_bytes((ROOT/'steam_auth_ui_probe.js').read_bytes())
    if interactive:
        (destination/'login.js').write_bytes((ROOT/'steam_auth_ui_login.js').read_bytes())
    scripts = ['libraries/libraries~00299a408.js', bundle,
               'chunk~1a96cdf59.js', 'library.js', 'probe.js']
    html = '<!doctype html><meta charset="utf-8"><title>Steam authentication transport probe</title>'
    html += '<p id="status">Loading Valve authentication modules…</p>'
    html += ''.join(f'<script src="{name}"></script>' for name in scripts)
    (destination/'index.html').write_text(html)
    return records
