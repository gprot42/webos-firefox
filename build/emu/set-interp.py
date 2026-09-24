#!/usr/bin/env python3
"""Point 32-bit ELF executables at another dynamic loader, in place.

The emulator test build needs a newer glibc than the emulator has, so its
executables must load the bundled glibc's loader, also for processes Firefox
starts itself. patchelf is avoided (rewriting these binaries broke them on
ARM); the new path must fit into the existing PT_INTERP string, so it is short
(/tmp/ld32.so, a symlink the run script creates).
Usage: set-interp.py NEW_PATH FILE...
"""
import struct
import sys


def set_interp(path, new):
    with open(path, "r+b") as f:
        data = f.read(52)
        if data[:4] != b"\x7fELF" or data[4] != 1:
            return "not a 32-bit ELF"
        e_phoff, = struct.unpack_from("<I", data, 28)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
        for i in range(e_phnum):
            f.seek(e_phoff + i * e_phentsize)
            p_type, p_offset, _, _, p_filesz = struct.unpack("<IIIII", f.read(20))
            if p_type == 3:  # PT_INTERP
                raw = new.encode() + b"\0"
                if len(raw) > p_filesz:
                    return "new path longer than the old one"
                f.seek(p_offset)
                old = f.read(p_filesz).rstrip(b"\0").decode()
                f.seek(p_offset)
                f.write(raw.ljust(p_filesz, b"\0"))
                return "%s -> %s" % (old, new)
        return "no PT_INTERP (a library)"


for p in sys.argv[2:]:
    print("%s: %s" % (p, set_interp(p, sys.argv[1])))
