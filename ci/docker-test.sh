#!/usr/bin/env bash
set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-22}"
GXX_VERSION="${GXX_VERSION:-16}"
JOBS="${JOBS:-4}"

cd "$(dirname "${BASH_SOURCE[0]}")/.."

docker build --target test \
    --build-arg "LLVM_VERSION=${LLVM_VERSION}" \
    --build-arg "GXX_VERSION=${GXX_VERSION}" \
    --build-arg "JOBS=${JOBS}" \
    -t caide-test .

printf 'Linux tests passed (LLVM %s, g++ %s, JOBS=%s).\n' "$LLVM_VERSION" "$GXX_VERSION" "$JOBS"
