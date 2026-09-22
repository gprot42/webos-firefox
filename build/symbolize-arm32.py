#!/usr/bin/env python3
"""Resolve "library+0xoffset" lines from the adapter's crash handler.

The offsets are file offsets of Thumb return addresses; this maps each to a
virtual address, steps back to the call instruction and asks llvm-symbolizer.
Run inside ffbuild against the unstripped library, for example:
  podman exec ffbuild python3 /src/build/symbolize-arm32.py \
      /tmp/wayland-1.22.0/build-arm32/src/libwayland-client.so.0.22.0 0x5d47
libxul from mach package has no symbol table, so it resolves to ?? there.
"""
import subprocess, sys
L = sys.argv[1]
segs = [l.split() for l in subprocess.check_output(["llvm-readelf", "-lW", L], text=True).splitlines()
        if l.strip().startswith("LOAD")]
for a in sys.argv[2:]:
    off = int(a, 16)
    for s in segs:
        po, pv, fs = int(s[1], 16), int(s[2], 16), int(s[4], 16)
        if po <= off < po + fs:
            pc = ((off - po + pv) & ~1) - 2
            out = subprocess.check_output(["llvm-symbolizer", "--obj=" + L, "--inlining=false", "--demangle", hex(pc)], text=True).split("\n")
            print(a, out[0][:150], out[1].replace("/work/firefox/", ""))
            break
