"""Freeze a game's public Meta application identity; never account credentials."""
import re
from pathlib import Path
import xml.etree.ElementTree as ET

# Some Unity games pass their application ID from managed code rather than
# AndroidManifest.xml. Only audited APK bytes get a local-import fallback.
KNOWN_APPS = {'545b1401a1f298cc8794370eb37ef4d7acb43156789af1c0aded09f5b2531541': '2370815932930055'}
ANDROID = '{http://schemas.android.com/apk/res/android}'


def app_id(tree, fingerprint='', selected=''):
    tree = Path(tree)
    declared = ''
    try:
        manifest = ET.parse(tree/'AndroidManifest.xml').getroot()
        declared = next((m.get(ANDROID+'value', '') for m in manifest.iter('meta-data')
                         if m.get(ANDROID+'name') == 'com.oculus.sdk.appid'), '')
        if declared.startswith('@string/'):
            strings = ET.parse(tree/'res/values/strings.xml').getroot()
            declared = next((''.join(s.itertext()) for s in strings if s.get('name') == declared[8:]), '')
    except (OSError, ET.ParseError):
        pass
    declared = declared if re.fullmatch(r'[1-9][0-9]{0,39}', declared) else ''
    known = KNOWN_APPS.get(fingerprint, '')
    if selected and not re.fullmatch(r'[1-9][0-9]{0,39}', selected):
        raise ValueError('Invalid Meta application ID in build input.')
    candidates = {v for v in (declared, known, selected) if v}
    if len(candidates) > 1:
        raise ValueError('Meta application identity does not match the APK.')
    return next(iter(candidates), '')
