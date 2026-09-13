#!/bin/bash
# libfabric pingpong demo over the same RoCE fabric as run.sh.
#
# Usage:
#   run_libfabric.sh server [size]
#   run_libfabric.sh client <server-ip> [size]
#
# size defaults to 65536. Avoid -S all: the default RLIMIT_MEMLOCK (8MB)
# on these hosts is too small for the largest sizes fi_pingpong sweeps
# through with -S all, and fi_mr_reg() fails with ENOMEM partway through.
set -euo pipefail

DOMAIN="$(fi_info -p verbs 2>/dev/null | awk '/^ *domain: rocep/ {print $2; exit}')"
if [ -z "$DOMAIN" ]; then
    echo "No RoCE verbs domain (rocep*) found via fi_info" >&2
    exit 1
fi

ROLE="${1:?role required: server|client}"

if [ "$ROLE" = "server" ]; then
    SIZE="${2:-65536}"
    exec fi_pingpong -p verbs -d "$DOMAIN" -e msg -S "$SIZE" -I 1000
elif [ "$ROLE" = "client" ]; then
    SERVER_IP="${2:?client requires <server-ip>}"
    SIZE="${3:-65536}"
    exec fi_pingpong -p verbs -d "$DOMAIN" -e msg -S "$SIZE" -I 1000 "$SERVER_IP"
else
    echo "Unknown role: $ROLE" >&2
    exit 1
fi
