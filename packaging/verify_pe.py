"""Verify protection metadata on the actual Windows release executables."""

import argparse
from pathlib import Path
import struct
import sys


def verify(path, require_system32_imports=False):
    data = Path(path).read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("truncated PE structure")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    pe, = unpack("<I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine, section_count = unpack("<HH", pe + 4)
    optional_size, characteristics = unpack("<HH", pe + 20)
    optional = pe + 24
    magic, = unpack("<H", optional)
    if machine != 0x8664 or magic != 0x20B or optional_size < 240:
        raise ValueError("expected a Windows x64 PE32+ executable")
    if not characteristics & 0x2 or characteristics & 0x1:
        raise ValueError("image is not executable or has stripped relocations")
    dll_flags, = unpack("<H", optional + 70)
    if dll_flags & 0x160 != 0x160:
        raise ValueError("ASLR, high-entropy ASLR, or NX compatibility flag is missing")
    directory_count, = unpack("<I", optional + 108)
    if directory_count < 11:
        raise ValueError("required PE data directories are missing")

    sections = []
    for index in range(section_count):
        header = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw_offset = unpack("<IIII", header + 8)
        flags, = unpack("<I", header + 36)
        if raw_size and raw_offset + raw_size > len(data):
            raise ValueError("a section extends beyond the executable file")
        if flags & 0xA0000000 == 0xA0000000:
            raise ValueError("a section is both writable and executable")
        sections.append((rva, virtual_size, raw_offset, raw_size))

    def file_offset(rva, size):
        for section_rva, _, raw_offset, raw_size in sections:
            relative = rva - section_rva
            if 0 <= relative and relative + size <= raw_size:
                offset = raw_offset + relative
                if offset + size <= len(data):
                    return offset
        raise ValueError("PE directory does not refer to file-backed section data")

    relocation_rva, relocation_size = unpack("<II", optional + 112 + 5 * 8)
    if not relocation_rva or relocation_size < 8:
        raise ValueError("ASLR relocation data is missing")
    file_offset(relocation_rva, relocation_size)

    if require_system32_imports:
        config_rva, config_size = unpack("<II", optional + 112 + 10 * 8)
        if not config_rva or config_size < 80:
            raise ValueError("load configuration for restricted DLL search is missing")
        config = file_offset(config_rva, 80)
        declared_size, = unpack("<I", config)
        dependent_flags, = unpack("<H", config + 78)
        if declared_size < 80 or dependent_flags != 0x800:
            raise ValueError("dependent DLL loads are not restricted to System32")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-system32-imports", action="store_true")
    parser.add_argument("executables", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.executables:
        try:
            verify(path, args.require_system32_imports)
        except (OSError, ValueError, struct.error) as error:
            print(f"{path}: {error}", file=sys.stderr)
            return 1
        print(f"{path}: PE protection checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
