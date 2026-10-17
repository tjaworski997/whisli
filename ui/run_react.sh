#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
export WHISLI_DEVICE_URL="${WHISLI_DEVICE_URL:-http://192.168.128.141}"
# Log in on the panel's own login screen. To skip it, set WHISLI_DEVICE_AUTH="whisli:<password>".

exec npm run dev -- --host 0.0.0.0
