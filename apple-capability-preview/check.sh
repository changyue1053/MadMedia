#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$0")/.."
root=$PWD
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
exec 3>&1
exec >"$work/check.log" 2>&1
stage=host-gate
trap 'printf "FAIL: %s\n" "$stage" >&3' ERR
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]
[[ $(xcodebuild -version | head -n 1) == 'Xcode 26.2' ]]
sdk=$(xcrun --sdk macosx --show-sdk-path)
[[ -d $sdk && $(xcrun --sdk macosx --show-sdk-version) == 26.2 ]]
mkdir -m 700 "$work/app-private"
stage=compile-lease
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -isysroot "$sdk" -arch arm64 \
  "$root/apple-capability-preview/lease_test.cpp" -o "$work/lease"
stage=compile-apple-entropy
xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -isysroot "$sdk" -arch arm64 \
  -I"$root/ffi/platform" "$root/apple-capability-preview/entropy_test.cpp" \
  "$root/ffi/platform/entropy_apple.cpp" -framework Security -o "$work/entropy"
stage=run-lease
"$work/lease" "$work/app-private"
printf 'PASS: native macOS ARM64 advisory lease fixture\n' >&3
stage=run-apple-entropy
"$work/entropy"
printf 'PASS: native macOS ARM64 public entropy provider with Security framework\n' >&3
