#!/usr/bin/env python3
"""Clear the executable-stack flag (PT_GNU_STACK p_flags &= ~PF_X) in ELF files.

hippo.so, libuic_io.so and libuic_jpeg.so from the Kodak driver are marked
RWE because the hand-written SSE assembly they are built from has no
.note.GNU-stack section.  Since glibc 2.41 the loader no longer makes the
stack executable on demand, so dlopen() of such a library fails outright with
"cannot enable executable stack as shared object requires: Invalid argument".

Clearing the flag is the same thing `execstack -c` / `patchelf
--clear-execstack` do; neither tool is packaged for Arch, hence this script.

Usage:  clear-execstack.py FILE...        (patches in place, idempotent)
        clear-execstack.py --check FILE...
"""
import struct
import sys

PT_GNU_STACK = 0x6474E551
PF_X = 1


def process(path, check_only):
    with open(path, 'rb' if check_only else 'r+b') as fh:
        data = fh.read(64)
        if data[:4] != b'\x7fELF':
            print(f'{path}: not an ELF file', file=sys.stderr)
            return 2
        if data[4] != 2:
            print(f'{path}: only ELF64 is supported', file=sys.stderr)
            return 2
        little = data[5] == 1
        end = '<' if little else '>'
        e_phoff, = struct.unpack_from(end + 'Q', data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from(end + 'HH', data, 0x36)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            fh.seek(off)
            hdr = fh.read(8)
            p_type, p_flags = struct.unpack(end + 'II', hdr)
            if p_type != PT_GNU_STACK:
                continue
            if not p_flags & PF_X:
                print(f'{path}: PT_GNU_STACK already non-executable '
                      f'(flags=0x{p_flags:x})')
                return 0
            if check_only:
                print(f'{path}: PT_GNU_STACK is EXECUTABLE (flags=0x{p_flags:x})')
                return 1
            fh.seek(off + 4)
            fh.write(struct.pack(end + 'I', p_flags & ~PF_X))
            print(f'{path}: cleared PF_X (0x{p_flags:x} -> '
                  f'0x{p_flags & ~PF_X:x})')
            return 0
        print(f'{path}: no PT_GNU_STACK segment')
        return 0


def main(argv):
    check_only = False
    args = []
    for a in argv:
        if a == '--check':
            check_only = True
        else:
            args.append(a)
    if not args:
        print(__doc__)
        return 2
    rc = 0
    for path in args:
        rc = max(rc, process(path, check_only))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
