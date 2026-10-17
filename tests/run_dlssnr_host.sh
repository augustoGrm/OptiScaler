#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tests/check_dlssnr_provenance.py
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/optiscaler-nr-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
g++ -std=c++20 -O1 -Wall -Wextra -pthread -Iexternal/simpleini \
  tests/dlssnr_settings.cpp OptiScaler/dlssnr/Settings.cpp -o "$test_dir/settings"
"$test_dir/settings"
for name in nr_guides_smoke nr_colour_encoding_smoke; do
  g++ -std=c++20 -O1 -Wall -Wextra "tests/$name.cpp" -o "$test_dir/$name"
  "$test_dir/$name"
done
