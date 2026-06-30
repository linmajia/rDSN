#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
bin="$script_dir/rasn.unit_tests"

echo "$bin --gtest_filter=rasn_*.*:codepilot_*.*"
"$bin" --gtest_filter='rasn_*.*:codepilot_*.*'
