#!/bin/sh
# CPU on the TV over a window (default 10 s): Firefox, the compositor and the
# rest of webOS, as % of one core, plus the whole TV. Every /proc/PID/stat is
# read by a single awk so both snapshots are near-instant; a per-process loop
# stretches the window on this busy TV and overstates by ~1.5x.
# Usage: ./scripts/ffcpu.sh [seconds]   (TV_IP/SSH_KEY as elsewhere)
TV_IP=${TV_IP:-192.168.0.79}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/webos_deploy}
W=${1:-10}
ssh -o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@$TV_IP" "W=$W"'
ff=""; for p in /proc/[0-9]*; do case "$(readlink $p/exe 2>/dev/null)" in */firefox-runtime/firefox*) ff="$ff ${p#/proc/}";; esac; done
SM=$(pidof surface-manager)
awk "{print \$1, \$14+\$15}" /proc/[0-9]*/stat 2>/dev/null > /tmp/c1; grep "^cpu " /proc/stat > /tmp/s1
sleep $W
awk "{print \$1, \$14+\$15}" /proc/[0-9]*/stat 2>/dev/null > /tmp/c2; grep "^cpu " /proc/stat > /tmp/s2
awk -v ff="$ff" -v sm="$SM" -v w=$W "BEGIN{n=split(ff,x,\" \"); for(i=1;i<=n;i++) isff[x[i]]=1}
  NR==FNR{a[\$1]=\$2;next} (\$1 in a){d=\$2-a[\$1]; if(\$1 in isff) f+=d; else if(\$1==sm) s+=d; else o+=d}
  END{printf \"  Firefox %3d%%   compositor %3d%%   rest of webOS %3d%%   (%% of one core; the TV has 4)\n\", f/w, s/w, o/w}" /tmp/c1 /tmp/c2
awk "NR==FNR{for(i=2;i<=NF;i++)a[i]=\$i;next}{t=0;for(i=2;i<=NF;i++)t+=\$i-a[i]; printf \"  whole TV: %d%% of total capacity\n\", (t-(\$5-a[5])-(\$6-a[6]))*100/t}" /tmp/s1 /tmp/s2'
