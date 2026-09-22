#!/bin/sh
# Put a compressed swap device in RAM ahead of the TV's flash swap partition.
# Stock webOS swaps to /dev/f2io-0 (flash) with vm.swappiness=100, which is
# what makes the browser stutter once memory is tight: every page-in is a
# flash read. zram (lz4) keeps swapped pages in RAM at roughly a third of
# their size. Runtime only; the TV returns to stock on reboot. To persist,
# copy this file to /var/lib/webosbrew/init.d/65-geckotv-zram on the TV.
SIZE_MB=${SIZE_MB:-512}
[ -e /sys/block/zram0/disksize ] || exit 0
[ "$(cat /sys/block/zram0/disksize)" = "0" ] || exit 0   # already set up
echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null || echo lzo-rle > /sys/block/zram0/comp_algorithm
echo $((SIZE_MB * 1024 * 1024)) > /sys/block/zram0/disksize
mkswap /dev/zram0 >/dev/null 2>&1 && swapon -p 10 /dev/zram0
echo 60 > /proc/sys/vm/swappiness
