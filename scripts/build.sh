#!/usr/bin/env bash
set -euo pipefail

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found. Activate your ESP-IDF environment first." >&2
  echo "See README.md (Quick start) for install/activation instructions." >&2
  exit 1
fi

idf.py set-target esp32s3 >/dev/null
idf.py build
