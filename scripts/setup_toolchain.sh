#!/usr/bin/env bash
# Installs the cross toolchain and TI SDK under ~/ti (no sudo).
# Usage: scripts/setup_toolchain.sh [path/to/mspm0_sdk_2_11_00_07.run]
set -euo pipefail
PREFIX="${HOME}/ti"
GCC_DIR="arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi"
GCC_URL="https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/${GCC_DIR}.tar.xz"
SDK_DIR="mspm0_sdk_2_11_00_07"
SDK_RUN="${1:-$(dirname "$0")/../${SDK_DIR}.run}"

mkdir -p "${PREFIX}"
if [ ! -x "${PREFIX}/${GCC_DIR}/bin/arm-none-eabi-gcc" ]; then
    echo "Downloading ${GCC_DIR}..."
    curl -L --fail -o "${PREFIX}/armgcc.tar.xz" "${GCC_URL}"
    tar -C "${PREFIX}" -xf "${PREFIX}/armgcc.tar.xz"
    rm "${PREFIX}/armgcc.tar.xz"
fi
if [ ! -d "${PREFIX}/${SDK_DIR}" ]; then
    if [ ! -f "${SDK_RUN}" ]; then
        echo "TI SDK installer not found at ${SDK_RUN}; download ${SDK_DIR}.run from ti.com" >&2
        exit 1
    fi
    chmod +x "${SDK_RUN}"
    "${SDK_RUN}" --mode unattended --prefix "${PREFIX}"
fi
"${PREFIX}/${GCC_DIR}/bin/arm-none-eabi-gcc" --version | head -1
echo "SDK: ${PREFIX}/${SDK_DIR}"
