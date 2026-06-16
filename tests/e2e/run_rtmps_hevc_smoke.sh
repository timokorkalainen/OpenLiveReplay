#!/usr/bin/env bash
# Native RTMPS HEVC e2e: same Enhanced RTMP HEVC assertions, over TLS.
set -uo pipefail

HARNESS="${1:?record_harness executable path required}"
RTMPS_PORT="${2:-23771}"
HERE="$(cd "$(dirname "$0")" && pwd)"

command -v openssl >/dev/null || { echo "SKIP: openssl not found"; exit 77; }

WORKDIR="$(mktemp -d)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

CERT="$WORKDIR/rtmps-hevc-cert.pem"
KEY="$WORKDIR/rtmps-hevc-key.pem"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1" \
    -keyout "$KEY" -out "$CERT" >/dev/null 2>&1 || {
        echo "SKIP: could not generate RTMPS self-signed certificate"
        exit 77
    }

REAL_PYTHON3="$(command -v python3 || true)"
if [ -z "$REAL_PYTHON3" ]; then
    echo "SKIP: python3 not found"
    exit 77
fi

mkdir -p "$WORKDIR/bin"
cat >"$WORKDIR/bin/python3" <<'PYTHON_SHIM'
#!/usr/bin/env bash
if [ "${1##*/}" = "rtmp_fixture_server.py" ] \
    && [ -n "${RTMP_SERVER_TLS_CERT:-}" ] \
    && [ -n "${RTMP_SERVER_TLS_KEY:-}" ]; then
    exec "$REAL_PYTHON3" "$@" --tls-cert "$RTMP_SERVER_TLS_CERT" --tls-key "$RTMP_SERVER_TLS_KEY"
fi
exec "$REAL_PYTHON3" "$@"
PYTHON_SHIM
chmod +x "$WORKDIR/bin/python3"

PATH="$WORKDIR/bin:$PATH" \
REAL_PYTHON3="$REAL_PYTHON3" \
RTMP_SCHEME=rtmps \
RTMP_SERVER_TLS_CERT="$CERT" \
RTMP_SERVER_TLS_KEY="$KEY" \
OLR_NATIVE_RTMP_ALLOW_INSECURE_TLS=1 \
    bash "$HERE/run_rtmp_hevc_smoke.sh" "$HARNESS" "$RTMPS_PORT"
