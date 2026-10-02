"""Exact-source bootstrap argument correction for the Walkabout ARM64 SDK.

The supplied original stays unchanged. An ignored translation input copy changes
one LDP operand pair so GetISteamUser receives (user, pipe), as its public ABI
requires. This does not change factory returns, authentication or ticket code.
"""
import hashlib
import struct

SHA256 = 'f424277c8a431542b93bf71215615fa6730edb7968a03f5230d484437f0d4aa4'
ADDRESS = 0x24624
ORIGINAL = 0x29400aa1  # ldp w1, w2, [x21]; SDK struct holds pipe then user.
REPLACEMENT = 0x294006a2  # ldp w2, w1, [x21]; public getter takes user then pipe.


def correct_bootstrap(content):
    if hashlib.sha256(content).hexdigest() != SHA256:
        raise ValueError('SDK bootstrap correction requires the audited original hash')
    if content[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', content, 18)[0] != 183:
        raise ValueError('SDK bootstrap correction requires ARM64 ELF')
    phoff = struct.unpack_from('<Q', content, 32)[0]
    entsize, count = struct.unpack_from('<HH', content, 54)
    matches = []
    for index in range(count):
        kind, flags, offset, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', content, phoff + index * entsize)
        if kind == 1 and flags & 1 and address <= ADDRESS and ADDRESS + 4 <= address + size:
            matches.append(offset + ADDRESS - address)
    if len(matches) != 1:
        raise ValueError('SDK bootstrap instruction is not in one executable segment')
    offset = matches[0]
    if struct.unpack_from('<I', content, offset)[0] != ORIGINAL:
        raise ValueError('SDK bootstrap operand instruction mismatch')
    corrected = bytearray(content)
    struct.pack_into('<I', corrected, offset, REPLACEMENT)
    return bytes(corrected), {'kind':'bootstrap_user_pipe_operand_order', 'virtual_address':hex(ADDRESS),
        'file_offset':offset, 'original_instruction':hex(ORIGINAL), 'translated_instruction':hex(REPLACEMENT),
        'original_sha256':SHA256, 'translation_input_sha256':hashlib.sha256(corrected).hexdigest()}
