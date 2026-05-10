#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)

KERNEL_VERSION=${KERNEL_VERSION:-5.4.49}
KERNEL_DIR=${KERNEL_DIR:-"${REPO_ROOT}/resources/linux-${KERNEL_VERSION}"}
JOBS=${JOBS:-$(nproc)}

CONFIG_URL="https://gem5.googlesource.com/public/gem5-resources/+/refs/heads/stable/src/linux-kernel/linux-configs/config.x86.${KERNEL_VERSION}?format=TEXT"
LINUX_REPO="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"

if ! command -v git >/dev/null 2>&1; then
    echo "error: git is required" >&2
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required" >&2
    exit 1
fi

if ! command -v base64 >/dev/null 2>&1; then
    echo "error: base64 is required" >&2
    exit 1
fi

if [ ! -d "${KERNEL_DIR}/.git" ]; then
    mkdir -p "$(dirname -- "${KERNEL_DIR}")"
    git clone --depth 1 --branch "v${KERNEL_VERSION}" "${LINUX_REPO}" "${KERNEL_DIR}"
fi

echo "vmx-demo: fetching gem5 x86 Linux ${KERNEL_VERSION} config"
curl -fsSL "${CONFIG_URL}" | base64 -d > "${KERNEL_DIR}/.config"

echo "vmx-demo: preparing/building Linux ${KERNEL_VERSION} in ${KERNEL_DIR}"
make -C "${KERNEL_DIR}" olddefconfig
make -C "${KERNEL_DIR}" -j"${JOBS}"

echo "vmx-demo: building external VMX demo module"
make -C "${KERNEL_DIR}" M="${SCRIPT_DIR}" modules

echo "vmx-demo: built ${SCRIPT_DIR}/vmx_shadow_vmcs_demo.ko"
