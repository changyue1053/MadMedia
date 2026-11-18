#!/usr/bin/env bash
# 一键：配置 + 构建 + 全量测试。用法: scripts/bootstrap.sh [build_dir] [-DIMMD_ASAN=ON ...]
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
shift || true
cmake -S . -B "$BUILD" "$@"
cmake --build "$BUILD" -j
ctest --test-dir "$BUILD" --output-on-failure

