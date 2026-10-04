"""Signing policy must preserve streaming capabilities and explicit overrides."""
import pathlib
import plistlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "visionos"))
from entitlements import OPTIONAL_ENTITLEMENTS, select_entitlements


class EntitlementsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.authored = plistlib.loads((pathlib.Path(__file__).resolve().parents[1]
                                      / "visionos/Klepton.entitlements").read_bytes())

    def test_steamlink_preserves_streaming_and_memory(self):
        selected, dropped = select_entitlements(self.authored, "steamlink-vr", {})
        self.assertEqual(selected, self.authored)
        self.assertEqual(dropped, [])

    def test_standalone_defaults_preserve_increased_memory_only(self):
        selected, dropped = select_entitlements(self.authored, "beatsaber", {})
        self.assertEqual(selected, {"com.apple.developer.kernel.increased-memory-limit": True})
        self.assertCountEqual(dropped, OPTIONAL_ENTITLEMENTS)

    def test_each_capability_can_be_disabled_independently(self):
        for key, variable in OPTIONAL_ENTITLEMENTS.items():
            with self.subTest(variable=variable):
                selected, dropped = select_entitlements(self.authored, "steamlink-vr", {variable: "0"})
                self.assertNotIn(key, selected)
                self.assertEqual(len(selected), len(self.authored) - 1)
                self.assertEqual(dropped, [key])
                self.assertIn(key, self.authored)

    def test_standalone_can_opt_in(self):
        selected, dropped = select_entitlements(self.authored, "superhot",
                                               {v: "1" for v in OPTIONAL_ENTITLEMENTS.values()})
        self.assertEqual(selected, self.authored)
        self.assertEqual(dropped, [])


if __name__ == "__main__":
    unittest.main()
