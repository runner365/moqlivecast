#!/bin/bash
# 用「忽略自签证书错误」的独立 profile 启动 Chrome，用于测试 WebTransport。
#
# 说明：Chrome 的 WebTransport(QUIC) 握手不走 --ignore-certificate-errors，
#       必须用 --ignore-certificate-errors-spki-list 指定要豁免的证书 SPKI 指纹。
#       SPKI 指纹对应 cert/server_cert.pem，可用下面的命令重新计算：
#         openssl x509 -in cert/server_cert.pem -noout -pubkey \
#           | openssl pkey -pubin -outform DER \
#           | openssl dgst -sha256 -binary | base64

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
PAGE="http://127.0.0.1:5173"
PROFILE="/tmp/chrome-wt-profile"
SPKI="ROg5jd/XH8v0mk9asnz8YgUesaUGhKBvA47Z52wKlSM="

if [ ! -x "$CHROME" ]; then
  echo "未找到 Chrome：$CHROME" >&2
  exit 1
fi

echo "启动 Chrome（独立 profile，忽略自签证书）..."
exec "$CHROME" \
  --ignore-certificate-errors-spki-list="$SPKI" \
  --user-data-dir="$PROFILE" \
  "$PAGE"
