#!/usr/bin/env bash
# Local build helper for the pdf-extractor project.
# Uses a self-contained Qt5 + zlib dev tree (no sudo required).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
SDK=/tmp/qtsdk/root/usr
QT_LIB="$SDK/lib/x86_64-linux-gnu"
QT_INC="$SDK/include/x86_64-linux-gnu/qt5"

cmake "$ROOT" -B "$BUILD" \
  -DMY_SDK="$SDK" \
  -DCMAKE_PREFIX_PATH="$SDK;$(dirname "$QT_LIB")" \
  -DCMAKE_INCLUDE_PATH="$QT_INC;$SDK/include;$QT_LIB" \
  -DCMAKE_LIBRARY_PATH="$QT_LIB" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$(dirname "$QT_LIB")" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD" -j"$(nproc)"
echo "=== build ok: $BUILD/pdfx ==="