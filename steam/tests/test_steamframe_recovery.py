"""Corruption/bounds tests for recovery-image inspection; no Valve downloads."""
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location('steamframe_recovery', Path(__file__).resolve().parents[2]/'steam/tools/steamframe_recovery.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


def fixture(sector=512, last=10):
    data = bytearray(sector*4)
    data[sector:sector+8] = b'EFI PART'
    struct.pack_into('<I',data,sector+12,92)
    struct.pack_into('<Q',data,sector+72,2)
    struct.pack_into('<II',data,sector+80,1,128)
    entry = sector*2
    data[entry] = 1
    struct.pack_into('<QQ',data,entry+32,4,last)
    name = 'rootfs-A'.encode('utf-16le'); data[entry+56:entry+56+len(name)] = name
    struct.pack_into('<I',data,sector+88,zlib.crc32(data[entry:entry+128]))
    struct.pack_into('<I',data,sector+16,zlib.crc32(data[sector:sector+92]))
    return data


class RecoveryInspection(unittest.TestCase):
    def test_sector_sizes(self):
        for sector in (512,4096):
            part = recovery.partitions(fixture(sector),sector*20)[0]
            self.assertEqual((part['name'],part['offset'],part['size']),('rootfs-A',sector*4,sector*7))

    def test_header_corruption(self):
        data = fixture(); data[512+20] ^= 1
        with self.assertRaisesRegex(ValueError,'header CRC'): recovery.partitions(data,20*512)

    def test_entry_corruption(self):
        data = fixture(); data[1024+56] ^= 1
        with self.assertRaisesRegex(ValueError,'entries CRC'): recovery.partitions(data,20*512)

    def test_truncated_table(self):
        with self.assertRaisesRegex(ValueError,'entries CRC'): recovery.partitions(fixture()[:1050],20*512)

    def test_partition_outside_image(self):
        with self.assertRaisesRegex(ValueError,'outside image'): recovery.partitions(fixture(last=25),20*512)

    def test_no_gpt(self):
        with self.assertRaisesRegex(ValueError,'no GPT'): recovery.partitions(bytes(8192),20*512)

    def test_posix_filenames(self):
        self.assertEqual(str(recovery.posix_member(r'dev-usb\x2dffs.mount')),r'dev-usb\x2dffs.mount')
        for name in ('../outside','/outside',''):
            with self.assertRaises(ValueError): recovery.posix_member(name)

    def test_member_bounds_and_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); target = root/'artifact'
            item = recovery.write_member(io.BytesIO(b'abc'),target,3,root)
            self.assertEqual(item['size'],3)
            with self.assertRaisesRegex(ValueError,'oversize'): recovery.write_member(io.BytesIO(b'abcd'),target,3,root)
            self.assertEqual(target.read_bytes(),b'abc')
            with self.assertRaisesRegex(ValueError,'truncated'): recovery.write_member(io.BytesIO(b'ab'),target,3,root)
            with self.assertRaisesRegex(ValueError,'unsafe'): recovery.write_member(io.BytesIO(b'a'),root/'..'/'outside',1,root)
            link = root/'link'; link.symlink_to(target)
            with self.assertRaisesRegex(ValueError,'unsafe'): recovery.write_member(io.BytesIO(b'a'),link,1,root)

if __name__ == '__main__': unittest.main()
