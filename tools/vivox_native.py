#!/usr/bin/env python3
"""Build the pinned public Vivox visionOS SDK as an embedded dynamic framework."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
LOCK = Path(__file__).with_suffix('.lock.json')
FRAMEWORK = ROOT/'visionos/Frameworks/KleptonVivox.xcframework'


def supports_tree(tree):
    if not tree:
        return False
    try:
        tree = Path(tree)
        package = ET.parse(tree/'AndroidManifest.xml').getroot().get('package')
        version = re.search(r"^\s*versionCode:\s*['\"]?(\d+)['\"]?\s*$",
                            (tree/'apktool.yml').read_text(), re.M)
        return package == 'com.LuckyVR.PokerStarsVR' and version is not None and version[1] == '1102036'
    except (OSError, ET.ParseError):
        return False


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def build():
    lock = json.loads(LOCK.read_text())
    cache = ROOT/'build/vivox-native'/lock['version']
    cache.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.check_output(['xcrun','--sdk','xros','--show-sdk-path'], text=True).strip()
    compiler = subprocess.check_output(['xcrun','clang++','--version'], text=True)
    key = hashlib.sha256((LOCK.read_text()+sdk+compiler).encode()+Path(__file__).read_bytes()).hexdigest()
    receipt = cache/'framework.json'
    binary = FRAMEWORK/'xros-arm64/KleptonVivox.framework/KleptonVivox'
    if receipt.exists() and binary.exists():
        prior = json.loads(receipt.read_text())
        if prior.get('key') == key and prior.get('sha256') == digest(binary):
            print('Native Vivox visionOS framework is current.', flush=True)
            return
    missing = [name for name, item in lock['files'].items()
               if not (cache/name).exists() or digest(cache/name) != item['sha256']]
    if missing:
        archive = cache/'sdk.tgz'
        if not archive.exists() or digest(archive) != lock['sha256']:
            print('Downloading the public Vivox SDK (cached for later builds)…', flush=True)
            temporary = cache/'sdk.tgz.part'
            with urllib.request.urlopen(lock['url'], timeout=60) as source, temporary.open('wb') as output:
                shutil.copyfileobj(source, output)
            if digest(temporary) != lock['sha256']:
                temporary.unlink()
                raise RuntimeError('Vivox SDK archive checksum mismatch')
            temporary.replace(archive)
        with tarfile.open(archive) as package:
            for name in missing:
                item = lock['files'][name]
                with package.extractfile(item['member']) as source, (cache/name).open('wb') as output:
                    shutil.copyfileobj(source, output)
                if digest(cache/name) != item['sha256']:
                    raise RuntimeError('Vivox SDK native input checksum mismatch: '+name)
    print('Linking native Vivox for physical Vision Pro…', flush=True)
    with tempfile.TemporaryDirectory(dir=cache, prefix='framework-') as temporary:
        work = Path(temporary)
        framework = work/'KleptonVivox.framework'
        framework.mkdir()
        args = ['xcrun','--sdk','xros','clang++','-dynamiclib','-target','arm64-apple-xros2.0',
                '-isysroot',sdk,'-Wl,-install_name,@rpath/KleptonVivox.framework/KleptonVivox',
                '-Wl,-force_load,'+str(cache/'libVivoxNative.a'),str(cache/'libvivoxsdk.a'),
                '-Wl,-dead_strip','-lz','-lresolv','-o',str(framework/'KleptonVivox')]
        for name in ['Foundation','AVFAudio','AudioToolbox','CoreAudio','CFNetwork','Security',
                     'SystemConfiguration','Accelerate','CoreFoundation','UIKit']:
            args += ['-framework',name]
        subprocess.run(args, check=True)
        info = dict(CFBundleIdentifier='dev.klepton.VivoxNative', CFBundleName='KleptonVivox',
                    CFBundleExecutable='KleptonVivox', CFBundlePackageType='FMWK',
                    CFBundleVersion=lock['version'], CFBundleShortVersionString=lock['version'],
                    CFBundleSupportedPlatforms=['XROS'], MinimumOSVersion='2.0')
        (framework/'Info.plist').write_bytes(plistlib.dumps(info))
        for name in ['PrivacyInfo.xcprivacy','LICENSE.md']:
            shutil.copyfile(cache/name, framework/name)
        output = work/'KleptonVivox.xcframework'
        subprocess.run(['xcodebuild','-create-xcframework','-framework',str(framework),
                        '-output',str(output)], check=True)
        FRAMEWORK.parent.mkdir(parents=True, exist_ok=True)
        if FRAMEWORK.exists():
            shutil.rmtree(FRAMEWORK)
        output.replace(FRAMEWORK)
    receipt.write_text(json.dumps(dict(key=key, sha256=digest(binary)), indent=2)+'\n')
    print('Native Vivox framework ready (physical visionOS slice).', flush=True)


if __name__ == '__main__':
    build()
