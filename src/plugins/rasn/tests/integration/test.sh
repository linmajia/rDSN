#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "SKIP: rASN multi-process integration harness is Linux-only"
    exit 0
fi

exec python3 "$script_dir/run_multinode.py" \
    --binary "$script_dir/codepilot" \
    --client-config "$script_dir/config.ini" \
    --runtime-config "$script_dir/config.rasn.ini" \
    --defaults-config "$script_dir/config.rasn.defaults.ini" \
    "$@"
