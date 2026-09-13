#!/bin/bash
# Usage:
#   run.sh server bw|lat|rate
#   run.sh client bw|lat|rate <server-ip>
set -euo pipefail

DEV="$(ibv_devices 2>/dev/null | awk '/rocep/ {print $1; exit}')"
if [ -z "$DEV" ]; then
    echo "No RoCE device (rocep*) found via ibv_devices" >&2
    exit 1
fi

ROLE="${1:?role required: server|client}"
TEST="${2:?test required: bw|lat|rate}"
SERVER_IP="${3:-}"

case "$TEST" in
    bw)   BIN="ib_write_bw"; ARGS="--report_gbits" ;;
    lat)  BIN="ib_write_lat"; ARGS="" ;;
    rate) BIN="ib_send_bw"; ARGS="--report_gbits -s 2 -n 100000" ;;
    *) echo "Unknown test: $TEST" >&2; exit 1 ;;
esac

if [ "$ROLE" = "server" ]; then
    exec $BIN -d "$DEV" $ARGS
elif [ "$ROLE" = "client" ]; then
    [ -n "$SERVER_IP" ] || { echo "client requires <server-ip>" >&2; exit 1; }
    exec $BIN -d "$DEV" $ARGS "$SERVER_IP"
else
    echo "Unknown role: $ROLE" >&2
    exit 1
fi
