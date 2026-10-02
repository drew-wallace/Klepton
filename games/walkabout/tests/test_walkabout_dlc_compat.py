"""Test the entitlement adapter's binary guards and single-call scope."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'games/walkabout/tools'))
import walkabout_dlc_compat as d
import walkabout_inspect as w

spec = importlib.util.spec_from_file_location('inspection_fixtures', Path(__file__).with_name('test_walkabout_inspect.py'))
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)


def fixture():
    data = fixtures.elf_fixture()
    source, original, ownership = 0x1180, 0x1200, 0x1240
    struct.pack_into('<I', data, source - 0x1000, d.branch_word(source, original))
    # Different wrapper bodies make accidental target substitution observable.
    struct.pack_into('<II', data, original - 0x1000, 0x52800000, 0xd65f03c0)
    struct.pack_into('<II', data, ownership - 0x1000, 0x52800020, 0xd65f03c0)
    binary = {'size': len(data), 'sha256': w.digest(data)}
    policy = {'consumer': 'fixture ownership consumer', 'callsite': source,
        'original_target': original, 'ownership_target': ownership,
        'method_regions': [dict(address=a, size=n, sha256=w.digest(data[a-0x1000:a-0x1000+n]))
                           for a, n in ((source, 4), (original, 8), (ownership, 8))]}
    return data, binary, policy


class DLCAdapterTests(unittest.TestCase):
    def test_only_consumer_branch_changes_not_license_queries(self):
        data, binary, policy = fixture()
        original = bytes(data)
        result, receipt = d.redirect_ownership(data, binary, policy)
        self.assertEqual(bytes(data), original)
        offset = policy['callsite'] - 0x1000
        expected = bytearray(data)
        struct.pack_into('<I', expected, offset, d.branch_word(policy['callsite'], policy['ownership_target']))
        self.assertEqual(result, expected)
        self.assertEqual(result[:offset], original[:offset])
        self.assertEqual(result[offset+4:], original[offset+4:])
        word = struct.unpack_from('<I', result, offset)[0]
        self.assertEqual(word & 0xfc000000, 0x14000000) # tail B, not BL or constant true
        imm = word & 0x3ffffff
        if imm & (1 << 25): imm -= 1 << 26
        self.assertEqual(policy['callsite'] + imm * 4, policy['ownership_target'])
        self.assertFalse(receipt['license_results_modified'])
        self.assertEqual(receipt['translation_input_sha256'], w.digest(result))

    def test_unknown_truncated_and_already_derived_inputs_fail(self):
        data, binary, policy = fixture()
        result, _ = d.redirect_ownership(data, binary, policy)
        changed = data.copy(); changed[-1] ^= 1
        for content in (changed, data[:-1], result):
            with self.subTest(size=len(content)), self.assertRaisesRegex(ValueError, 'original binary hash'):
                d.redirect_ownership(content, binary, policy)

    def test_wrong_instruction_rejected_even_with_matching_artifact_hash(self):
        data, binary, policy = fixture()
        struct.pack_into('<I', data, 0x180, 0xd503201f)
        binary['sha256'] = w.digest(data)
        policy['method_regions'][0]['sha256'] = w.digest(data[0x180:0x184])
        with self.assertRaisesRegex(ValueError, 'tail call mismatch'):
            d.redirect_ownership(data, binary, policy)

    def test_wrapper_audit_and_executable_targets_required(self):
        data, binary, policy = fixture()
        bad = copy.deepcopy(policy); bad['method_regions'][1]['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'method audit mismatch'):
            d.redirect_ownership(data, binary, bad)
        bad = copy.deepcopy(policy); bad['ownership_target'] = 0x2200
        with self.assertRaisesRegex(ValueError, 'not executable'):
            d.redirect_ownership(data, binary, bad)

    def test_branch_alignment_range_and_negative_displacement(self):
        self.assertEqual(d.branch_word(0x2000, 0x1000), 0x17fffc00)
        for source, target in ((0, 2), (1, 4), (0, 1 << 27), (1 << 27 | 4, 0)):
            with self.assertRaises(ValueError): d.branch_word(source, target)

    def test_preparation_repeatable_and_failed_attempt_preserves_output(self):
        data, binary, policy = fixture()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source, output, receipt = (root/n for n in ('original.so', 'derived.so', 'receipt.json'))
            source.write_bytes(data)
            lock = root/'lock.json'; lock.write_text(json.dumps({'binary': binary, 'dlc_compatibility': policy}))
            with patch.object(d, 'LOCK', lock):
                d.prepare(source, output, receipt)
                first = output.read_bytes(), receipt.read_bytes()
                d.prepare(source, output, receipt)
                self.assertEqual(first, (output.read_bytes(), receipt.read_bytes()))
                self.assertEqual(source.read_bytes(), data)
                with self.assertRaises(ValueError): d.prepare(source, source, receipt)
                source.write_bytes(b'invalid')
                with self.assertRaises(ValueError): d.prepare(source, output, receipt)
                self.assertEqual(first, (output.read_bytes(), receipt.read_bytes()))


if __name__ == '__main__':
    unittest.main()
