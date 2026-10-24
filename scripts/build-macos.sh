#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$repo/build/macos"}
cmake=${CMAKE:-cmake}
if ! command -v "$cmake" >/dev/null 2>&1 && [ -x "$repo/deps/python-tools/cmake/data/bin/cmake" ]; then
  cmake="$repo/deps/python-tools/cmake/data/bin/cmake"
fi
python3 "$repo/scripts/fetch-macos-deps.py"
"$cmake" -S "$repo/src/desktop" -B "$build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' \
  -DCMAKE_PREFIX_PATH="$repo/deps/Qt-6.10.2-macos"
"$cmake" --build "$build" --parallel 4
"$(dirname "$(command -v "$cmake")")/ctest" --test-dir "$build" --output-on-failure
QT_QPA_PLATFORM=offscreen "$build/mark-six-studio.app/Contents/MacOS/mark-six-studio" \
  --smoke --data-dir "$build/smoke-data"
QT_QPA_PLATFORM=offscreen "$build/mark-six-studio.app/Contents/MacOS/mark-six-studio" \
  --app-update-audit --data-dir "$build/update-audit-data"
QT_QPA_PLATFORM=offscreen "$build/mark-six-studio.app/Contents/MacOS/mark-six-studio" \
  --storage-cleanup-audit --data-dir "$build/cleanup-audit-data"
