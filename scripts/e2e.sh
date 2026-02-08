#!/usr/bin/env bash
set -euo pipefail

# Stable E2E entrypoint. Environment/device settings are loaded from:
#   tools/e2e/e2e.local.env (gitignored)
# Template:
#   tools/e2e/e2e.example.env

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONUNBUFFERED=1
python3 -u "${ROOT_DIR}/tools/e2e/test_rs485_stub.py" --ensure-stub
