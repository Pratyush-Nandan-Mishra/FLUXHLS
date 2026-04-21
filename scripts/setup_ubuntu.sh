#!/usr/bin/env bash
# FluxHLS — Ubuntu / Debian setup script
# Installs LLVM + libclang, configures, and builds the project.
# Tested on Ubuntu 20.04, 22.04, 24.04.
#
# Usage:
#   chmod +x scripts/setup_ubuntu.sh
#   ./scripts/setup_ubuntu.sh
#
# To force a specific LLVM version (default: auto-detect best available):
#   LLVM_VERSION=17 ./scripts/setup_ubuntu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ── Detect or choose LLVM version ─────────────────────────────────────────────

if [[ -z "${LLVM_VERSION:-}" ]]; then
    # Pick the highest installed version, or fall back to 17
    for v in 18 17 16 15 14; do
        if command -v "llvm-config-${v}" &>/dev/null; then
            LLVM_VERSION=$v
            break
        fi
    done
    LLVM_VERSION="${LLVM_VERSION:-17}"
fi

echo "[FluxHLS] Using LLVM version: ${LLVM_VERSION}"

# ── Install dependencies ───────────────────────────────────────────────────────

echo "[FluxHLS] Installing build tools..."
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential cmake wget gnupg lsb-release ca-certificates

# Check if the chosen LLVM version is already available
if ! apt-cache show "llvm-${LLVM_VERSION}-dev" &>/dev/null; then
    echo "[FluxHLS] LLVM ${LLVM_VERSION} not in default repos — adding LLVM APT repo..."
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | \
        sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc > /dev/null
    CODENAME="$(lsb_release -sc)"
    echo "deb http://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-${LLVM_VERSION} main" | \
        sudo tee "/etc/apt/sources.list.d/llvm-${LLVM_VERSION}.list"
    sudo apt-get update -qq
fi

echo "[FluxHLS] Installing LLVM ${LLVM_VERSION} + libclang..."
sudo apt-get install -y --no-install-recommends \
    "llvm-${LLVM_VERSION}-dev" \
    "libclang-${LLVM_VERSION}-dev" \
    "clang-${LLVM_VERSION}"

# Confirm header and library are present
LLVM_ROOT="/usr/lib/llvm-${LLVM_VERSION}"
if [[ ! -f "${LLVM_ROOT}/include/clang-c/Index.h" ]]; then
    echo "[FluxHLS] ERROR: clang-c/Index.h not found in ${LLVM_ROOT}/include"
    exit 1
fi
echo "[FluxHLS] Found: ${LLVM_ROOT}/include/clang-c/Index.h"

# ── Configure and build ────────────────────────────────────────────────────────

BUILD_DIR="${PROJECT_DIR}/build"
echo "[FluxHLS] Configuring in ${BUILD_DIR}..."
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_INSTALL_DIR="${LLVM_ROOT}"

echo "[FluxHLS] Building..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo ""
echo "══════════════════════════════════════════════════════"
echo " Build complete!  Binary: ${BUILD_DIR}/fluxhls"
echo ""
echo " Try it:"
echo "   ${BUILD_DIR}/fluxhls ${PROJECT_DIR}/test/vadd.cpp"
echo "   ${BUILD_DIR}/fluxhls ${PROJECT_DIR}/test/fir.cpp"
echo "   ${BUILD_DIR}/fluxhls ${PROJECT_DIR}/test/matmul.cpp"
echo "══════════════════════════════════════════════════════"
