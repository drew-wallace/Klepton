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
BETA_ENTRY = 'var Un=ze.O(void 0,[41],()=>ze(43519));Un=ze.O(Un)'
# The 2026-11-13 UI moves the HTTPS transport and folds the RSA helper into
# Valve's login module. Preserve the wrapper's small existing module interface;
# encryption still runs through Valve's exported password/key function.
BETA_LOADER = '''globalThis.KleptonValveRequire=id=>id===79853?{D:ze(19099).x}:
id===36500?{A:{getPublicKey:(publickey_mod,publickey_exp)=>({publickey_mod,publickey_exp}),
encrypt:ze(53366).P8}}:ze(id)'''


def expose_loader(main):
    known = [(ENTRY, 'globalThis.KleptonValveRequire=ve'), (BETA_ENTRY, BETA_LOADER)]
    matching = [(entry, loader) for entry, loader in known if main.count(entry)]
    if len(matching) != 1 or main.count(matching[0][0]) != 1:
        raise ValueError('Unrecognized Valve UI bootstrap')
    entry, loader = matching[0]
    return main.replace(entry, loader)


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
    # Only expose the existing loader. Do not launch the desktop library shell.
    checked['library.js'] = expose_loader(main).encode('utf-8')
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
