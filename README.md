# gem5 Intel VMX support

This fork of gem5 adds a research-oriented Intel VMX model for x86 full-system
simulation. It focuses on a testable VMX lifecycle and the paging machinery
needed for Extended Page Tables (EPT) guest execution.

## Current status

- VMXON/VMXOFF, VMCS management, VMREAD/VMWRITE, VMLAUNCH/VMRESUME, and VMCALL.
- 64-bit guest entry and exit, selected intercepts and event injection, VMCS
  validation, and checkpoint/restore of VMX state.
- Reworked x86 paging and MMU paths, with a memory-backed EPT walker connected
  to guest page-table reads, guest A/D writes, and final instruction/data
  translations. Atomic and timing walks use conditional physical EPT A/D
  updates; functional inspection remains read-only. These paths are tested
  with suspended-CPU fixtures. **EPT is not yet enabled for guest execution.**

## Remaining work

- [ ] Enable EPT at VM entry and validate EPT exit delivery during guest execution.
- [ ] Complete EPT memory attributes, invalidation, and end-to-end tests
      before advertising EPT support.
- [ ] Expand VMX coverage beyond the current 64-bit, single-vCPU subset
      (including optional controls and multi-vCPU behavior).
- [ ] Calibrate VMX instruction timing against hardware.

See the [VMX audit](docs/vmx-architectural-audit.md) and
[EPT status](docs/x86-paging-ept-status.md) for the detailed scope and gaps.

## Build and test

From the repository root, build gem5, the disk-free test guests, and the x86
unit tests:

```sh
scons build/X86/gem5.opt \
  build/X86/arch/x86/vmx_utils.test.opt \
  build/X86/arch/x86/vmcs.test.opt \
  build/X86/arch/x86/paging.test.opt \
  build/X86/arch/x86/ept.test.opt -j"$(nproc)"
make -C tests/gem5/x86_paging
```

Run the unit tests and simulator regressions:

```sh
for test in vmx_utils vmcs paging ept; do
  build/X86/arch/x86/$test.test.opt
done
python3 tests/gem5/x86_paging/run.py
python3 tests/gem5/x86_paging/run.py --transport-only --outdir m5out-paging-transport
python3 tests/gem5/x86_paging/run.py --ept-only --outdir m5out-ept-walker
python3 tests/gem5/x86_paging/run.py --vmx-operands-only --outdir m5out-vmx-operands
python3 tests/gem5/x86_paging/vmresume.py
```

The Linux full-system VMX smoke and transition tests require separate kernel
resources. Follow [their setup guide](tests/gem5/vmx/README.md), then run
`tests/gem5/vmx/run.sh` or `tests/gem5/vmx/run.sh transition`.

The [paging test guide](tests/gem5/x86_paging/README.md) documents individual
cases, options, and expected results.
