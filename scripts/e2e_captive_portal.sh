#!/usr/bin/env bash
set -euo pipefail

# Stable entrypoint for the separate real-device captive portal verification.
# Environment/device settings are loaded by the Python script from:
#   tools/e2e/e2e.local.json (preferred, gitignored)
#   tools/e2e/e2e.local.env (gitignored)
#   .secrets (gitignored, local only)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONUNBUFFERED=1
LOG_DIR="${ROOT_DIR}/tools/e2e/logs"
mkdir -p "${LOG_DIR}"
TS="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${LOG_DIR}/portal_e2e_${TS}.log"
LATEST_LOG="${LOG_DIR}/portal_e2e_latest.log"

echo "[e2e_captive_portal.sh] writing log: ${LOG_FILE}"
status=0
python3 -u "${ROOT_DIR}/tools/e2e/test_captive_portal.py" 2>&1 | tee "${LOG_FILE}" || status=$?
cp -f "${LOG_FILE}" "${LATEST_LOG}"
exit "${status}"
