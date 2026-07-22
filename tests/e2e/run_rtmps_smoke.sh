#!/usr/bin/env bash
# Native RTMPS e2e: same media assertions as RTMP smoke, over TLS transport.
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
RTMPS_PORT="${2:-23860}"
HERE="$(cd "$(dirname "$0")" && pwd)"

command -v openssl >/dev/null || { echo "SKIP: openssl not found"; exit 77; }

WORKDIR="$(mktemp -d)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

CERT="$WORKDIR/rtmps-cert.pem"
KEY="$WORKDIR/rtmps-key.pem"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1" \
    -keyout "$KEY" -out "$CERT" >/dev/null 2>&1 || {
        echo "SKIP: could not generate RTMPS self-signed certificate"
        exit 77
    }

RTMP_SCHEME=rtmps \
RTMP_SERVER_TLS_CERT="$CERT" \
RTMP_SERVER_TLS_KEY="$KEY" \
OLR_NATIVE_RTMP_ALLOW_INSECURE_TLS=1 \
    bash "$HERE/run_rtmp_smoke.sh" "$HARNESS" "$RTMPS_PORT"
