#!/bin/bash
# Silent until the Firefox build finishes or the log goes stale.
LOG=/Users/aicoder/src/public/webos-firefox/build/linux-build.log
STALE_SECS=5400
while true; do
    if [[ -f $LOG ]]; then
        last=$(grep -E '^(BUILD_DONE |BUILD_FAILED$)' "$LOG" | tail -1 || true)
        case $last in
            BUILD_DONE\ *) echo "DONE"; exit 0 ;;
            BUILD_FAILED) echo "FAILED: build script reported BUILD_FAILED"; exit 1 ;;
        esac
    fi
    if ! podman exec ffbuild true >/dev/null 2>&1; then
        echo "FAILED: ffbuild container is not running"
        exit 1
    fi
    if [[ -f $LOG ]]; then
        now=$(date +%s)
        mtime=$(stat -f %m "$LOG")
        if (( now - mtime > STALE_SECS )); then
            echo "FAILED: build log stale for ${STALE_SECS}s"
            exit 1
        fi
    fi
    sleep 30
done
