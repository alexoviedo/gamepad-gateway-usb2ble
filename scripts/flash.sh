#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-}"

if [[ -z "${PORT}" ]]; then
  echo "Usage: $0 <serial-port>" >&2
  echo "Examples:" >&2
  echo "  $0 /dev/ttyUSB0" >&2
  echo "  $0 /dev/cu.usbmodem101" >&2
  echo "  $0 COM3" >&2
  exit 2
fi

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found. Activate your ESP-IDF environment first." >&2
  exit 1
fi

idf.py -p "${PORT}" flash monitor
