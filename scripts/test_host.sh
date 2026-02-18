#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/host"
REPORTER="summary"

if [[ "${1:-}" == "--console" ]]; then
    REPORTER="console"
    shift
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}"
"${BUILD_DIR}/host_tests" --duration "--reporters=${REPORTER}" "$@"
