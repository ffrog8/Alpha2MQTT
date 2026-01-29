#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="alpha2mqtt-host-tests"

DOCKERFILE_PATH="${ROOT_DIR}/scripts/Dockerfile.test"

docker build -f "${DOCKERFILE_PATH}" -t "${IMAGE_NAME}" "${ROOT_DIR}"
docker run --rm -v "${ROOT_DIR}:/repo" -w /repo "${IMAGE_NAME}" ./scripts/test_host.sh
