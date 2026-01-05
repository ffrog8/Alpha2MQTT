#!/usr/bin/env bash
set -euo pipefail

if ! command -v gh >/dev/null 2>&1; then
  apt-get update
  apt-get install -y gh
fi

if ! gh auth status >/dev/null 2>&1; then
  if [ -z "${GH_TOKEN:-}" ]; then
    echo "GH_TOKEN is not set; unable to authenticate gh." >&2
    exit 1
  fi
  echo "$GH_TOKEN" | gh auth login --with-token
fi
