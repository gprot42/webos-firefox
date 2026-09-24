#!/bin/bash
# List glibc symbols, with their versions, that files need from a glibc newer
# than 2.12 (the nc4 sysroot's). A shared library links even when it needs
# such symbols, and only fails when a TV with an old glibc loads it, so run
# this on everything that ships. The loader checks versions, not just names:
# exp@GLIBC_2.29 fails on an old glibc even though exp itself exists there.
# Usage (inside ffbuild):  bash /src/build/nc4/glibc-check.sh <dirs or files>
set -euo pipefail
MAX=${GLIBC_MAX:-2.12}

files=()
for a in "$@"; do
    if [ -d "$a" ]; then
        while IFS= read -r f; do files+=("$f"); done < <(find "$a" -type f)
    elif [ -f "$a" ]; then
        files+=("$a")
    else
        echo "missing: $a"
        exit 2
    fi
done
checked=0

bad=0
for f in "${files[@]}"; do
    file -b "$f" | grep -q "ELF 32-bit" || continue
    checked=$((checked + 1))
    too_new=$(objdump -T "$f" 2>/dev/null |
              awk -v max="$MAX" '
                  function newer(v, m,   a, b, i) {
                      split(v, a, "."); split(m, b, ".")
                      for (i = 1; i <= 3; i++) {
                          if ((a[i] + 0) > (b[i] + 0)) return 1
                          if ((a[i] + 0) < (b[i] + 0)) return 0
                      }
                      return 0
                  }
                  match($0, /GLIBC_[0-9.]+/) {
                      v = substr($0, RSTART + 6, RLENGTH - 6)
                      if (newer(v, max)) print $NF "@" v
                  }' | sort -u | tr '\n' ' ')
    if [ -n "$too_new" ]; then
        echo "$(basename "$f"): $too_new"
        bad=1
    fi
done
[ "$checked" -eq 0 ] && { echo "no 32-bit ELF files found"; exit 2; }
[ "$bad" -eq 0 ] && echo "all $checked files need glibc $MAX or older"
exit "$bad"
