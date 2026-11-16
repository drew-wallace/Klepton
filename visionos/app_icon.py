"""Build local visionOS icon stacks from apktool-decoded launcher resources.

Uses only Python's standard library and the Mac's Swift/ImageIO toolchain. Game
art stays in ignored build output; manually supplied asset stacks take priority
in gen_xcodeproj.py.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ANDROID = '{http://schemas.android.com/apk/res/android}'
INFO = {'author': 'xcode', 'version': 1}
DENSITIES = {'ldpi': 120, 'mdpi': 160, 'hdpi': 240, 'xhdpi': 320,
             'xxhdpi': 480, 'xxxhdpi': 640, 'nodpi': 160, 'anydpi': 160}


class IconError(ValueError):
    pass


class Resources:
    def __init__(self, tree):
        self.root = Path(tree) / 'res'

    def candidates(self, kind, name):
        candidates = []
        for folder in self.root.glob(kind + '*'):
            parts = folder.name.split('-')
            # Pick the default appearance, independent of locale, orientation,
            # device size, or night mode. Version/density variants are usable.
            if parts[0] != kind or any(p not in DENSITIES and not re.fullmatch(r'v\d+', p)
                                       for p in parts[1:]):
                continue
            density = next((DENSITIES[p] for p in parts[1:] if p in DENSITIES), 160)
            for path in folder.glob(name + '.*'):
                area = 0
                if path.suffix.lower() == '.png':
                    with path.open('rb') as f:
                        header = f.read(24)
                    if len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n':
                        continue
                    width, height = struct.unpack('>II', header[16:24])
                    area = width * height
                candidates.append((area, density, str(path), path))
        return [p for _, _, _, p in sorted(candidates, reverse=True)]

    def resolve(self, reference, seen=()):
        if not reference or reference in seen or len(seen) >= 16:
            raise IconError('Missing or cyclic launcher resource')
        if reference.startswith('#'):
            value = reference[1:]
            if len(value) in (3, 4):
                value = ''.join(c * 2 for c in value)
            if not re.fullmatch(r'[0-9a-fA-F]{6}|[0-9a-fA-F]{8}', value):
                raise IconError('Invalid icon color')
            if len(value) == 6:
                value = 'ff' + value
            a, r, g, b = [int(value[i:i+2], 16) / 255 for i in range(0, 8, 2)]
            return {'color': [r, g, b, a]}
        match = re.fullmatch(r'@(drawable|mipmap|color)/([A-Za-z0-9_.$]+)', reference)
        if not match:
            raise IconError('Unsupported launcher resource: ' + reference)
        kind, name = match.groups()
        seen = (*seen, reference)
        candidates = self.candidates(kind, name)
        # Prefer adaptive layers when they can both be decoded. Otherwise use
        # the legacy bitmap with the same name, shipped by most Android apps.
        for path in [p for p in candidates if p.suffix == '.xml']:
            try:
                node = ET.parse(path).getroot()
                if node.tag == 'adaptive-icon':
                    layers = [self.resolve(node.find(tag).get(ANDROID + 'drawable'), seen)
                              for tag in ('background', 'foreground')]
                    if all('path' in layer or 'color' in layer for layer in layers):
                        return {'background': layers[0], 'foreground': layers[1]}
                elif node.tag == 'bitmap':
                    return self.resolve(node.get(ANDROID + 'src'), seen)
            except (IconError, ET.ParseError, AttributeError):
                continue
        for path in candidates:
            if path.suffix.lower() in ('.png', '.webp', '.jpg', '.jpeg'):
                return {'path': str(path.resolve())}
        # Resource aliases and color values live in values/*.xml, rather than
        # drawable files. Resolve only the unqualified default configuration.
        for path in sorted((self.root / 'values').glob('*.xml')):
            try:
                nodes = ET.parse(path).getroot()
            except ET.ParseError:
                continue
            for node in nodes:
                if node.get('name') == name and (node.get('type') or node.tag) == kind:
                    return self.resolve((node.text or '').strip(), seen)
        raise IconError('No supported image for ' + reference)


def icon_layers(tree):
    tree = Path(tree)
    manifest = tree / 'AndroidManifest.xml'
    if not manifest.is_file():
        return None
    try:
        application = ET.parse(manifest).getroot().find('application')
    except ET.ParseError as exc:
        raise IconError('Cannot read launcher manifest') from exc
    if application is None:
        return None
    resources = Resources(tree)
    error = None
    for attribute in ('icon', 'roundIcon'):
        reference = application.get(ANDROID + attribute)
        if not reference:
            continue
        try:
            source = resources.resolve(reference)
            if 'background' in source:
                return source
            if 'path' in source:
                return {'background': source, 'foreground': {}}
        except IconError as exc:
            error = exc
    if error:
        raise error
    return None


def renderer(build_dir):
    source = Path(__file__).with_name('render_app_icon.swift')
    stamp = hashlib.sha256(source.read_bytes()).hexdigest()[:16]
    executable = Path(build_dir) / 'icon-tools' / ('render-' + stamp)
    if not executable.is_file():
        executable.parent.mkdir(parents=True, exist_ok=True)
        # Publish only a complete executable, including when builds overlap.
        with tempfile.TemporaryDirectory(dir=executable.parent) as temp:
            output = Path(temp) / 'render'
            subprocess.run(['xcrun', '--sdk', 'macosx', 'swiftc', str(source), '-O',
                            '-o', str(output)], check=True, capture_output=True, timeout=120)
            output.replace(executable)
    return executable


def generate_icon(tree, catalog, name, build_dir):
    """Return True if artwork was found. Failures leave no partial asset stack."""
    layers = icon_layers(tree)
    if layers is None:
        return False
    catalog = Path(catalog)
    catalog.mkdir(parents=True, exist_ok=True)
    executable = renderer(build_dir)
    with tempfile.TemporaryDirectory(prefix='icon-', dir=catalog.parent) as temp:
        temp = Path(temp)
        stack = temp / (name + '.solidimagestack')
        stack.mkdir()
        (stack / 'Contents.json').write_text(json.dumps({
            'info': INFO, 'layers': [{'filename': layer + '.solidimagestacklayer'}
                                    for layer in ('Front', 'Back')]}))
        for layer in ('Front', 'Back'):
            folder = stack / (layer + '.solidimagestacklayer')
            images = folder / 'Content.imageset'
            images.mkdir(parents=True)
            (folder / 'Contents.json').write_text(json.dumps({'info': INFO}))
            (images / 'Contents.json').write_text(json.dumps({
                'info': INFO, 'images': [{'idiom': 'vision', 'scale': '2x', 'filename': 'icon.png'}]}))
            layers['front_output' if layer == 'Front' else 'back_output'] = str(images / 'icon.png')
        spec = temp / 'render.json'
        spec.write_text(json.dumps(layers))
        subprocess.run([str(executable), str(spec)], check=True, capture_output=True, timeout=30)
        destination = catalog / stack.name
        if destination.exists():
            shutil.rmtree(destination)
        stack.replace(destination)
    return True
