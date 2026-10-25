#!/usr/bin/env bash
# Start py_ws_echo + wt2ws_proxy for wt2ws_smoke.
#   ./wt2ws/run_demo.sh
# Ctrl-C stops both.  For browser testing use StreamVox:
#   cd wt2ws/StreamVox/web && npm run dev
set -euo pipefail
cd "$(dirname "$0")/.."

# The demo always uses the bundled mkcert cert (URL is 127.0.0.1), so ignore
# any machine-level MOQ_SSL_CERT_FILE/MOQ_SSL_KEY_FILE.
unset MOQ_SSL_CERT_FILE MOQ_SSL_KEY_FILE

BIN=./build/wt2ws/wt2ws_proxy
if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN; build first:"
  echo "  cmake --build build --target wt2ws_proxy wt2ws_smoke"
  exit 1
fi

python3 wt2ws/py_ws_echo/echo_server.py --port 9001 &
ECHO_PID=$!
"$BIN" --backend ws://127.0.0.1:9001/ --console &
PROXY_PID=$!

cleanup() {
  kill "$ECHO_PID" "$PROXY_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep 1
cat <<'TXT'
wt2ws demo up:
  py_ws_echo : ws://127.0.0.1:9001/
  proxy      : wt://0.0.0.0:5433/wt2ws
  smoke      : ./build/wt2ws/wt2ws_smoke 127.0.0.1 5433 /wt2ws
  browser    : cd wt2ws/StreamVox/web && npm run dev  (http://127.0.0.1:5173)
Ctrl-C to stop.
TXT
wait
