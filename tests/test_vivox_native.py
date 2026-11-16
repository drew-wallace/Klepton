"""Limit the native SDK replacement to the inspected game release."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import vivox_native


class NativeVoiceScopeTests(unittest.TestCase):
    def test_package_and_version_must_both_match(self):
        with tempfile.TemporaryDirectory() as directory:
            tree=Path(directory)
            for package,version,expected in [('com.LuckyVR.PokerStarsVR',1102036,True),
                                              ('com.LuckyVR.PokerStarsVR',1102037,False),
                                              ('com.example.other',1102036,False)]:
                (tree/'AndroidManifest.xml').write_text(f'<manifest package="{package}"/>')
                (tree/'apktool.yml').write_text(f"versionInfo:\n  versionCode: '{version}'\n")
                self.assertEqual(vivox_native.supports_tree(tree),expected)

    def test_missing_or_malformed_metadata_does_not_enable_sdk(self):
        self.assertFalse(vivox_native.supports_tree(None))
        with tempfile.TemporaryDirectory() as directory:
            tree=Path(directory)
            self.assertFalse(vivox_native.supports_tree(tree))
            (tree/'AndroidManifest.xml').write_text('<manifest broken')
            (tree/'apktool.yml').write_text('versionCode: 1102036\n')
            self.assertFalse(vivox_native.supports_tree(tree))


if __name__=='__main__':
    unittest.main()
