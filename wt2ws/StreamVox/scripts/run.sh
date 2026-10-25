#!/usr/bin/env bash
# Start StreamVox from the project root.
#
# moqlivecast/wt2ws use MOQ_SSL_CERT_FILE / MOQ_SSL_KEY_FILE now, so the
# standard Python/OpenSSL SSL_CERT_FILE is left alone for CA bundles.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -n "${SSL_CERT_FILE:-}" ]]; then
  echo "note: SSL_CERT_FILE is set; moqlivecast now uses MOQ_SSL_CERT_FILE." >&2
  echo "      unset SSL_CERT_FILE SSL_KEY_FILE if it points to a WebTransport bundle." >&2
fi

if [[ ! -x .venv/bin/python ]]; then
  echo "missing .venv; create it first:" >&2
  echo "  python3.10 -m venv .venv" >&2
  echo "  .venv/bin/pip install -e '.[dev]'" >&2
  exit 1
fi

exec .venv/bin/python -m streamvox "$@"
