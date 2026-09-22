#!/bin/sh
# Build the smoke IPK and install it on the rooted TV when it is reachable.
# Usage: ./scripts/install2tvfrommacos.sh
#        TV_IP=192.168.0.79 ./scripts/install2tvfrommacos.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TV_IP=${TV_IP:-192.168.0.79}
DEVICE=${DEVICE:-webos}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/webos_deploy}
APP_ID=com.github.gprot42.geckotv
LAUNCH=${LAUNCH:-1}

cd "$ROOT"
chmod 755 app/geckotv.sh
python3 scripts/make-icons.py
make
python3 scripts/pack-ipk.py

# Take the version from appinfo.json; a hard-coded name here once pointed at
# a stale 0.1.0 package while newer ones sat beside it in dist/.
VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$ROOT/app/appinfo.json")
IPK=$ROOT/dist/${APP_ID}_${VERSION}_arm.ipk
if [ ! -f "$IPK" ]; then
    echo "package missing: $IPK" >&2
    exit 1
fi

if [ ! -f "$SSH_KEY" ]; then
    echo "built $IPK"
    echo "SSH key $SSH_KEY is missing, so the TV was not updated."
    exit 0
fi

if ! ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
    -i "$SSH_KEY" "root@${TV_IP}" true >/dev/null 2>&1; then
    echo "built $IPK"
    echo "root@${TV_IP} did not answer. Install later with:"
    echo "  ares-install -d $DEVICE $IPK"
    exit 0
fi

# ares-install's SSH channel is flaky against this TV's dropbear.
if ! ares-install -d "$DEVICE" "$IPK"; then
    remote="/media/developer/temp/$(basename "$IPK")"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@${TV_IP}" \
        "mkdir -p /media/developer/temp && rm -f '$remote'"
    scp -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" \
        "$IPK" "root@${TV_IP}:$remote"
    payload=$(printf '{"id":"com.ares.defaultName","ipkUrl":"%s","subscribe":true}' "$remote")
    # Old luna-send-pub on this firmware takes the URI and payload as positional args.
    ssh -tt -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@${TV_IP}" \
        "/usr/bin/luna-send-pub -n 60 -w 90000 'luna://com.webos.appInstallService/dev/install' '$payload'"
fi

if [ "$LAUNCH" = "1" ]; then
    ssh -tt -o BatchMode=yes -o StrictHostKeyChecking=no -i "$SSH_KEY" "root@${TV_IP}" \
        "/usr/bin/luna-send-pub -n 1 -w 15000 'luna://com.webos.applicationManager/launch' '{\"id\":\"$APP_ID\"}'" \
        || true
fi

echo "installed $IPK on $TV_IP"
