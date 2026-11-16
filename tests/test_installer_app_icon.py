"""Launcher artwork selection and real Mac/visionOS asset compilation."""
import contextlib
import io
import os
from pathlib import Path
import plistlib
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'visionos'))
import app_icon
import targets


def png(size, rgba=(255, 80, 40, 255)):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgba) * size) * size)) + chunk(b'IEND', b''))


class IconTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='game icon `literal` $ ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.tree = self.root / 'decoded'
        self.tree.mkdir()
        self.manifest('@mipmap/launcher')

    def manifest(self, icon, extra=''):
        (self.tree / 'AndroidManifest.xml').write_text(
            '<manifest xmlns:android="http://schemas.android.com/apk/res/android">'
            f'<application android:icon="{icon}" {extra}/></manifest>')

    def resource(self, folder, name, data):
        path = self.tree / 'res' / folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        return path

    def test_largest_default_bitmap_wins_over_density_and_localized_variants(self):
        self.resource('mipmap-xxxhdpi', 'launcher.png', png(16))
        largest = self.resource('mipmap-xhdpi', 'launcher.png', png(64))
        self.resource('mipmap-fr-xxxhdpi', 'launcher.png', png(128))
        self.resource('mipmap-night', 'launcher.png', png(128))
        self.assertEqual(app_icon.icon_layers(self.tree), {
            'background': {'path': str(largest)}, 'foreground': {}})

    def adaptive(self, foreground='@drawable/logo'):
        self.resource('mipmap-anydpi-v26', 'launcher.xml',
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">'
            '<background android:drawable="@color/brand"/>'
            f'<foreground android:drawable="{foreground}"/></adaptive-icon>')
        self.resource('values', 'colors.xml', '<resources><color name="brand">#8044aaff</color></resources>')

    def test_adaptive_bitmap_and_argb_color_stay_separate(self):
        self.adaptive()
        logo = self.resource('drawable-xxhdpi', 'logo.png', png(64, (0, 0, 255, 128)))
        self.resource('mipmap-xxxhdpi', 'launcher.png', png(32))
        layers = app_icon.icon_layers(self.tree)
        self.assertEqual(layers['background']['color'], [0x44/255, 0xaa/255, 1, 0x80/255])
        self.assertEqual(layers['foreground'], {'path': str(logo)})

    def test_vector_adaptive_falls_back_to_legacy_bitmap(self):
        self.adaptive()
        self.resource('drawable', 'logo.xml', '<vector/>')
        legacy = self.resource('mipmap-xxhdpi', 'launcher.png', png(64))
        self.assertEqual(app_icon.icon_layers(self.tree)['background']['path'], str(legacy))

    def test_resource_alias_and_bitmap_reference(self):
        self.resource('values', 'aliases.xml',
            '<resources><item type="mipmap" name="launcher">@drawable/wrapper</item></resources>')
        self.resource('drawable', 'wrapper.xml',
            '<bitmap xmlns:android="http://schemas.android.com/apk/res/android" android:src="@drawable/art"/>')
        art = self.resource('drawable', 'art.png', png(16))
        self.assertEqual(app_icon.icon_layers(self.tree)['background']['path'], str(art))

    def test_cyclic_alias_uses_round_icon_without_recursing_forever(self):
        self.manifest('@mipmap/launcher', 'android:roundIcon="@drawable/round"')
        self.resource('values', 'aliases.xml',
            '<resources><item type="mipmap" name="launcher">@mipmap/launcher</item></resources>')
        art = self.resource('drawable', 'round.png', png(16))
        self.assertEqual(app_icon.icon_layers(self.tree)['background']['path'], str(art))

    def test_missing_or_unsupported_artwork_does_not_create_partial_stack(self):
        with self.assertRaises(app_icon.IconError):
            app_icon.generate_icon(self.tree, self.root/'Icons.xcassets', 'AppIcon-test', self.root/'build')
        self.assertFalse((self.root/'Icons.xcassets').exists())
        (self.tree/'AndroidManifest.xml').unlink()
        self.assertFalse(app_icon.generate_icon(self.tree, self.root/'Icons.xcassets', 'AppIcon-test', self.root/'build'))

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'Requires Mac Xcode tools')
    def test_render_failure_preserves_existing_stack(self):
        self.resource('mipmap', 'launcher.png', png(16))
        stack = self.root/'Icons.xcassets/AppIcon-test.solidimagestack'
        stack.mkdir(parents=True)
        (stack/'marker').write_text('existing')
        with patch.object(app_icon, 'renderer', return_value='/usr/bin/false'):
            with self.assertRaises(subprocess.CalledProcessError):
                app_icon.generate_icon(self.tree, stack.parent, 'AppIcon-test', self.root/'build')
        self.assertEqual((stack/'marker').read_text(), 'existing')
        self.assertEqual(list(self.root.glob('icon-*')), [])

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'Requires Mac Xcode tools')
    def test_real_renderer_and_visionos_compiler_accept_adaptive_and_flat_icons(self):
        self.adaptive()
        self.resource('drawable', 'logo.png', png(64, (0, 0, 255, 128)))
        catalog = self.root/'Icons.xcassets'
        for adaptive in (True, False):
            with self.subTest(adaptive=adaptive):
                if not adaptive:
                    self.manifest('@drawable/logo')
                name = 'AppIcon-test'
                self.assertTrue(app_icon.generate_icon(self.tree, catalog, name, ROOT/'visionos/build'))
                for layer in ('Front', 'Back'):
                    file = catalog/(name+'.solidimagestack')/(layer+'.solidimagestacklayer')/'Content.imageset/icon.png'
                    self.assertEqual(struct.unpack('>II', file.read_bytes()[16:24]), (1024, 1024))
                output = self.root/('compiled-' + str(adaptive))
                output.mkdir()
                subprocess.run(['xcrun', 'actool', str(catalog), '--compile', str(output),
                                '--platform', 'xros', '--minimum-deployment-target', '26.0',
                                '--target-device', 'vision', '--app-icon', name,
                                '--output-partial-info-plist', str(output/'Info.plist')],
                               check=True, capture_output=True, timeout=60)
                info = plistlib.loads((output/'Info.plist').read_bytes())
                self.assertEqual(info['CFBundleIcons']['CFBundlePrimaryIcon'], name)
                self.assertTrue((output/'Assets.car').is_file())

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'Requires Mac Xcode tools')
    def test_project_uses_imported_tree_and_keeps_custom_icon_priority(self):
        self.resource('mipmap-xxxhdpi', 'launcher.png', png(64))
        # A selected compatibility profile must never supply another APK's art.
        key = 'icon-test-' + uuid.uuid4().hex[:12]
        target = dict(targets.TARGETS['superhot'], name=key, host_tree=str(self.tree),
                      libs='libmain', bundle='test.game.icon', product='IconTest')
        catalog = ROOT/'visionos/build/appicon'/(key+'.xcassets')
        self.addCleanup(lambda: shutil.rmtree(catalog, ignore_errors=True))
        asset = ROOT/'visionos/Assets.xcassets'/('AppIcon-'+key+'.solidimagestack')
        self.addCleanup(lambda: shutil.rmtree(asset, ignore_errors=True))
        with patch.object(targets, 'resolve', return_value=target), patch.dict(os.environ, {
                'KLEPTON_TEAM': 'TESTTEAM00', 'KLEPTON_STEAM_LOCAL': '0',
                'KLEPTON_BUILD_MANIFEST': '', 'KL_CUSTOM_LAUNCHER': '0'}), contextlib.redirect_stdout(io.StringIO()):
            project = runpy.run_path(str(ROOT/'visionos/gen_xcodeproj.py'))
            self.assertTrue(project['HAS_ICON'])
            self.assertIn(f'KL_APPICON_NAME = "AppIcon-{key}"', project['PBX'])
            self.assertTrue((catalog/asset.name/'Back.solidimagestacklayer/Content.imageset/icon.png').exists())
            # Existing handmade icons bypass extraction and are copied intact.
            asset.mkdir()
            (asset/'marker').write_text('handmade icon')
            with patch.object(app_icon, 'generate_icon', side_effect=AssertionError('custom icon replaced')):
                project = runpy.run_path(str(ROOT/'visionos/gen_xcodeproj.py'))
            self.assertTrue(project['HAS_ICON'])
            self.assertEqual((catalog/asset.name/'marker').read_text(), 'handmade icon')


if __name__ == '__main__':
    unittest.main()
