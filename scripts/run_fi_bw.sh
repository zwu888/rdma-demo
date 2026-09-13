#!/bin/bash
# Pipelined libfabric bandwidth demo (src/fi_bw.cpp). Unlike fi_pingpong,
# this keeps multiple sends/recvs outstanding (like ib_write_bw's TX
# depth), so it measures real saturation bandwidth instead of being
# capped by round-trip latency. Build first: (cd src && make)
#
# Usage:
#   run_fi_bw.sh server [size] [window] [iters]
#   run_fi_bw.sh client <server-ip> [size] [window] [iters]
set -euo pipefail

DOMAIN="$(fi_info -p verbs 2>/dev/null | awk '/^ *domain: rocep/ {print $2; exit}')"
if [ -z "$DOMAIN" ]; then
    echo "No RoCE verbs domain (rocep*) found via fi_info" >&2
    exit 1
fi

BIN="$(dirname "$0")/../src/fi_bw"
if [ ! -x "$BIN" ]; then
    echo "Build it first: (cd $(dirname "$0")/../src && make)" >&2
    exit 1
fi

ROLE="${1:?role required: server|client}"

if [ "$ROLE" = "server" ]; then
    SIZE="${2:-65536}"; WINDOW="${3:-16}"; ITERS="${4:-20000}"
    exec "$BIN" server -d "$DOMAIN" -s "$SIZE" -w "$WINDOW" -n "$ITERS"
elif [ "$ROLE" = "client" ]; then
    SERVER_IP="${2:?client requires <server-ip>}"
    SIZE="${3:-65536}"; WINDOW="${4:-16}"; ITERS="${5:-20000}"
    exec "$BIN" client "$SERVER_IP" -d "$DOMAIN" -s "$SIZE" -w "$WINDOW" -n "$ITERS"
else
    echo "Unknown role: $ROLE" >&2
    exit 1
fi
