#!/usr/bin/env python3
"""Rewrite absolute symlinks inside a sysroot so they resolve within it.
A Debian sysroot's libfoo.so -> /lib/arm-linux-gnueabi/libfoo.so.N would
otherwise point at the build machine's own /lib when used for cross linking."""
import os, sys
root = os.path.realpath(sys.argv[1])
fixed = broken = 0
for d, dirs, files in os.walk(root):
    for name in dirs + files:
        p = os.path.join(d, name)
        if not os.path.islink(p):
            continue
        target = os.readlink(p)
        if not target.startswith("/"):
            continue
        rel = os.path.relpath(os.path.join(root, target.lstrip("/")), d)
        os.remove(p)
        os.symlink(rel, p)
        fixed += 1
        if not os.path.exists(p):
            broken += 1
print(f"relinked {fixed} absolute symlinks, {broken} still dangling (expected for a few optional files)")
