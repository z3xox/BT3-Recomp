#!/bin/bash
# Compatible with the system bash 3.2; Python handles paths and bundle assembly.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$ROOT/tools/macos/deploy.py" "$@"
