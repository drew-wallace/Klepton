#!/usr/bin/env python3
"""Prepare an audited Walkabout ownership adapter for AOT translation.

Only IPlatformDLC.DoesUserOwnProduct's tail call changes: ask the genuine
SteamApps.BIsSubscribedApp wrapper instead of BIsDlcInstalled. Android course
bundles are supplied independently of Steam depot installation. Neither Steam
API implementation nor license results are changed. The supplied ELF stays
untouched; the derived input and receipt belong in ignored build/.
"""
import argparse
import json
from pathlib import Path
import struct
import tempfile

from walkabout_inspect import Arm64ELF, LOCK, ROOT, digest


def branch_word(source, target):
    displacement = target - source
    if source % 4 or target % 4 or not -(1 << 27) <= displacement < (1 << 27):
        raise ValueError('unaligned or out-of-range ARM64 branch')
    return 0x14000000 | ((displacement // 4) & 0x03ffffff)


def redirect_ownership(content, binary, policy):
    """Pure preparation helper; production policy is the tracked artifact lock."""
    if len(content) != binary['size'] or digest(content) != binary['sha256']:
        raise ValueError('DLC adapter requires the audited original binary hash')
    elf = Arm64ELF(content)
    # Audit the entire caller and both wrappers, not just a coincidentally
    # matching branch. The two static wrappers both take (AppId_t, MethodInfo*)
    # and return bool; the caller passes the product ID and a null MethodInfo.
    for method in policy['method_regions']:
        address, size = method['address'], method['size']
        if not elf.executable(address) or not elf.executable(address + size - 1):
            raise ValueError('DLC adapter method is not executable')
        offset = elf.raw(address, size)
        if digest(content[offset:offset + size]) != method['sha256']:
            raise ValueError('DLC adapter method audit mismatch')
    source, old_target, new_target = (policy[k] for k in ('callsite', 'original_target', 'ownership_target'))
    if not all(elf.executable(a) for a in (source, old_target, new_target)):
        raise ValueError('DLC adapter branch is not executable')
    offset = elf.raw(source, 4)
    original, replacement = branch_word(source, old_target), branch_word(source, new_target)
    if struct.unpack_from('<I', content, offset)[0] != original:
        raise ValueError('DLC adapter expected tail call mismatch')
    result = bytearray(content)
    struct.pack_into('<I', result, offset, replacement)
    result = bytes(result)
    receipt = {'kind': 'walkabout_dlc_ownership_query', 'target': 'walkabout-57013',
        'consumer': policy['consumer'], 'original_query': 'SteamApps.BIsDlcInstalled',
        'ownership_query': 'SteamApps.BIsSubscribedApp', 'callsite': hex(source),
        'original_target': hex(old_target), 'ownership_target': hex(new_target),
        'file_offset': offset, 'original_instruction': hex(original),
        'translated_instruction': hex(replacement), 'original_sha256': digest(content),
        'translation_input_sha256': digest(result), 'changed_instruction_count': 1,
        'license_results_modified': False, 'physical_course_loading_verified': False}
    return result, receipt


def prepare(source, output, receipt_path):
    lock = json.loads(LOCK.read_text())
    if len({source.resolve(), output.resolve(), receipt_path.resolve()}) != 3:
        raise ValueError('source, derived input and receipt must be separate files')
    result, receipt = redirect_ownership(source.read_bytes(), lock['binary'], lock['dlc_compatibility'])
    # Validate everything before replacing even an existing generated input.
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as temporary:
        temporary.write(result)
        temporary_path = Path(temporary.name)
    temporary_path.replace(output)
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'walkabout-57013/lib/arm64-v8a/libil2cpp.so')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/walkabout-dlc-compat/libil2cpp.so')
    parser.add_argument('--receipt', type=Path, default=ROOT / 'build/walkabout-dlc-compat/receipt.json')
    args = parser.parse_args()
    prepare(args.binary, args.out, args.receipt)
    print('Walkabout DLC adapter: genuine Steam subscription query; one audited tail call')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, struct.error) as error:
        raise SystemExit(f'Walkabout DLC preparation failed: {error}')
