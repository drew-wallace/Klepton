"""Record the APK version used for the installed executable, not staged data."""
import json
import plistlib
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path
import xml.etree.ElementTree as ET


def installed_version(tree, apk=None):
    result = apk_dates(tree, apk)
    try:
        text = (Path(tree) / 'apktool.yml').read_text()
    except OSError:
        return result
    block = re.search(r'^versionInfo:[ \t]*\n((?:[ \t]+[^\n]*\n?)+)', text, re.M)
    if not block:
        return result
    for field, key in (('versionName', 'KleptonGameVersion'),
                       ('versionCode', 'KleptonGameVersionCode')):
        match = re.search(r'^[ \t]+' + field + r':[ \t]*([^\n]*)', block[1], re.M)
        if not match:
            continue
        value = match[1].strip()
        if value.startswith('"'):
            try:
                value = json.loads(value)
            except ValueError:
                continue
        elif value.startswith("'"):
            if not value.endswith("'"):
                continue
            value = value[1:-1].replace("''", "'")
        if not isinstance(value, str) or not value or value in ('null', '~'):
            continue
        if any(ord(c) < 32 for c in value):
            continue
        if key.endswith('Code') and (not value.isascii() or not value.isdecimal()):
            continue
        result[key] = value
    return result


def normalized_date(value):
    """Only explicit ISO dates or Unix seconds; never infer from APK ZIP mtimes."""
    try:
        if isinstance(value, datetime):
            date = value
        elif isinstance(value, str) and re.fullmatch(r'\d{4}-\d{2}-\d{2}', value):
            date = datetime.fromisoformat(value)
            return value if 2008 <= date.year <= datetime.now(timezone.utc).year + 1 else None
        elif isinstance(value, str) and re.fullmatch(r'\d{10}', value):
            date = datetime.fromtimestamp(int(value), timezone.utc)
        else:
            date = datetime.fromisoformat(str(value).replace('Z', '+00:00'))
        if not 2008 <= date.year <= datetime.now(timezone.utc).year + 1:
            return None
        if date.tzinfo is None:
            date = date.replace(tzinfo=timezone.utc)
        return date.astimezone(timezone.utc).isoformat(timespec='seconds')
    except (ValueError, TypeError, OverflowError, OSError):
        return None


def downloaded_date(apk):
    if apk is None or not Path(apk).is_file():
        return None
    apk = str(Path(apk).resolve())
    try:
        output = subprocess.run(['/usr/bin/mdls', '-plist', '-', '-name',
                                 'kMDItemDownloadedDate', apk], capture_output=True,
                                timeout=3, check=True).stdout
        dates = plistlib.loads(output).get('kMDItemDownloadedDate', [])
        if isinstance(dates, datetime):
            dates = [dates]
        dates = sorted(filter(None, (normalized_date(d) for d in dates)))
        if dates:
            return dates[0]
    except (OSError, ValueError, TypeError, subprocess.SubprocessError):
        pass
    # macOS quarantine records the acquisition timestamp, including files
    # downloaded by sync clients which do not populate Spotlight's date.
    try:
        output = subprocess.run(['/usr/bin/xattr', '-p', 'com.apple.quarantine', apk],
                                capture_output=True, timeout=3, check=True).stdout.decode()
        fields = output.strip().split(';')
        if len(fields) >= 3 and re.fullmatch(r'[0-9a-fA-F]{8,16}', fields[1]):
            return normalized_date(datetime.fromtimestamp(int(fields[1], 16), timezone.utc))
    except (OSError, ValueError, OverflowError, subprocess.SubprocessError):
        pass
    return None


def apk_dates(tree, apk):
    result = {}
    android = '{http://schemas.android.com/apk/res/android}'
    try:
        manifest = ET.parse(Path(tree) / 'AndroidManifest.xml')
        # Optional publisher metadata. Android's platformBuildVersion is the
        # compile SDK version, and certificate/ZIP dates are not release dates.
        fields = {'releaseDate': 'Release', 'release_date': 'Release',
                  'buildDate': 'Build', 'build_date': 'Build'}
        for item in manifest.findall('./application/meta-data'):
            kind = fields.get(item.get(android + 'name'))
            date = normalized_date(item.get(android + 'value')) if kind else None
            if date:
                result['KleptonGame' + kind + 'Date'] = date
    except (OSError, ET.ParseError):
        pass
    if not result:
        date = downloaded_date(apk)
        if date:
            result['KleptonGameDownloadDate'] = date
    return result
