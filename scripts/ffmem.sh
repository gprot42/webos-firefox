#!/bin/sh
# Firefox memory on the TV, per process, matched on the executable so the
# renamed content processes ("Web Content") are counted. PSS divides shared
# pages fairly between processes; summing RSS overstates by ~100 MB.
# Usage: ./scripts/ffmem.sh   (runs over ssh, TV_IP/SSH_KEY as elsewhere)
TV_IP=${TV_IP:-192.168.0.79}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/webos_deploy}
ssh -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@$TV_IP" '
tot=0
for p in /proc/[0-9]*; do
  exe=$(readlink $p/exe 2>/dev/null) || continue
  case "$exe" in */firefox-runtime/firefox*) ;; *) continue;; esac
  [ -r $p/smaps_rollup ] || continue
  pss=$(awk "/^Pss:/{print \$2}" $p/smaps_rollup); sw=$(awk "/^Swap:/{print \$2}" $p/smaps_rollup)
  printf "  %5s MB pss  %4s MB swap  %s\n" "$((pss/1024))" "$((sw/1024))" "$(cat $p/comm)"
  tot=$((tot+pss))
done
echo "  TOTAL $((tot/1024)) MB"
free -m | awk "NR==2{print \"  system free \" \$4 \" MB, available \" \$7 \" MB\"} NR==3{print \"  swap used \" \$3 \"/\" \$2 \" MB\"}"'
