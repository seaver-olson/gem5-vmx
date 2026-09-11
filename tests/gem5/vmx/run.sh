#!/bin/bash
set -euo pipefail

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "${TEST_DIR}/../../.." && pwd)
GEM5=${GEM5:-"${REPO_ROOT}/build/X86/gem5.opt"}
MODE=${1:-normal}
CHECKPOINT_DIR=${VMX_CHECKPOINT_DIR:-"${REPO_ROOT}/m5out-vmx-checkpoint/cpt-vmx-active"}
NO_KVM_ARG=""
if [ "${VMX_NO_KVM:-0}" = 1 ]; then
    NO_KVM_ARG="--no-kvm"
fi

if [ ! -x "${GEM5}" ]; then
    echo "VMX smoke: missing ${GEM5}"
    echo "Build it with: scons build/X86/gem5.opt -j\"\$(nproc)\""
    exit 2
fi

make -C "${TEST_DIR}"

case "${MODE}" in
normal)
    OUTDIR="${REPO_ROOT}/m5out-vmx-smoke"
    EXTRA_ARGS=""
    RESULT_PREFIX="vmx_smoke"
    ;;
checkpoint)
    OUTDIR="${REPO_ROOT}/m5out-vmx-checkpoint"
    EXTRA_ARGS="--take-checkpoint ${CHECKPOINT_DIR}"
    RESULT_PREFIX="vmx_smoke"
    ;;
restore)
    OUTDIR="${REPO_ROOT}/m5out-vmx-restore"
    EXTRA_ARGS="--restore ${CHECKPOINT_DIR}"
    RESULT_PREFIX="vmx_smoke"
    ;;
transition)
    OUTDIR="${REPO_ROOT}/m5out-vmx-transition"
    EXTRA_ARGS="--module ${TEST_DIR}/vmx_transition.ko"
    RESULT_PREFIX="vmx_transition"
    ;;
*)
    echo "Usage: $0 [normal|checkpoint|restore|transition]"
    exit 2
    ;;
esac

if [ -n "${VMX_BOOT_CHECKPOINT:-}" ]; then
    if [ "${VMX_NO_KVM:-0}" != 1 ] || [ "${MODE}" != transition ]; then
        echo "VMX_BOOT_CHECKPOINT requires VMX_NO_KVM=1 and transition mode"
        exit 2
    fi
    EXTRA_ARGS="${EXTRA_ARGS} --restore-boot ${VMX_BOOT_CHECKPOINT}"
fi

mkdir -p "${OUTDIR}"
LOG="${OUTDIR}/console.log"
SERIAL_LOG="${OUTDIR}/board.pc.com_1.device"

# EXTRA_ARGS contains only paths chosen above; intentional word splitting lets
# argparse receive the option and path as separate arguments.
# shellcheck disable=SC2086
"${GEM5}" --outdir="${OUTDIR}" --debug-flags=VMX \
    --debug-file=vmx.trace "${TEST_DIR}/config.py" ${EXTRA_ARGS} \
    ${NO_KVM_ARG} \
    2>&1 | tee "${LOG}"

if ! grep -q "${RESULT_PREFIX}: PASS: COMPLETE" "${LOG}" "${SERIAL_LOG}"; then
    echo "VMX ${MODE}: FAIL (PASS marker absent from guest output)"
    exit 1
fi
if grep -q "${RESULT_PREFIX}: FAIL:" "${LOG}" "${SERIAL_LOG}"; then
    echo "VMX ${MODE}: FAIL (module reported failure)"
    exit 1
fi
if [ "${MODE}" = checkpoint ] && [ ! -f "${CHECKPOINT_DIR}/m5.cpt" ]; then
    echo "VMX smoke: FAIL (checkpoint missing: ${CHECKPOINT_DIR}/m5.cpt)"
    exit 1
fi

echo "VMX instruction results:"
sed -n "s/^.*${RESULT_PREFIX}: PASS: /  [PASS] /p" "${SERIAL_LOG}" |
    tr -d '\r' |
    awk '!seen[$0]++'
echo "VMX ${RESULT_PREFIX#vmx_}: PASS (${MODE})"
if [ "${MODE}" = checkpoint ]; then
    echo "Restore it with: $0 restore"
fi
