# Testing gem5-vmx

The maintained VMX tests are checked in under `tests/gem5/vmx`.  They execute
privileged instructions from loadable Linux modules after the full-system
harness switches from KVM fast-forward to gem5's Atomic CPU.  A host with
`/dev/kvm` makes boot fast; set `VMX_NO_KVM=1` for an all-Atomic run.

## Build and unit tests

The tested compiler on this host is GCC 14:

```sh
CC=gcc-14 CXX=g++-14 scons build/X86/gem5.opt \
    build/X86/arch/x86/vmcs.test.opt \
    build/X86/arch/x86/vmx_utils.test.opt -j"$(nproc)"

build/X86/arch/x86/vmcs.test.opt
build/X86/arch/x86/vmx_utils.test.opt
```

`vmcs.test.opt` protects field decoding, width/high access, read-only and
reserved encodings, lifecycle state, and serialization.  `vmx_utils.test.opt`
protects capability masks, fixed bits, CR3 validity, CR0 loaded-bit merging,
and distinct CLTS/LMSW rules.

## Root-operation and capability smoke

```sh
tests/gem5/vmx/run.sh normal
```

This verifies CPUID, the exact post-CPU-switch capability MSRs, read-only MSR
behavior, VMXON, VMCLEAR revision independence, VMPTRLD including error 11,
VMPTRST, VMREAD/VMWRITE, and VMXOFF.

## Architectural entry/exit and translation regression

```sh
tests/gem5/vmx/run.sh transition
```

This is the primary correctness regression.  The kernel module creates host,
guest-A, and guest-B page-table mappings from one linear address to three
different physical pages.  It verifies:

- guest RIP/RSP execution and the guest-A mapping after VMLAUNCH;
- a non-exiting MOV-to-CR3 and guest-B data/CR3 visibility;
- VMCALL state save, host CR3/mapping restoration, and VMRESUME;
- CR0 guest/host mask, read shadow, non-exiting CLTS, and CLTS exit metadata;
- configured CR3-load exit reason/qualification without changing guest CR3;
- directly intercepted #PF qualification/event fields without changing CR2;
- VMCLEAR current/launch behavior and VMfailInvalid/VMfailValid flags;
- committed late VM-entry failure, clear launch state, and preservation of
  unrelated exit fields; and
- atomic pre-commit control failure with error 7.

The test passes only on `vmx_transition: PASS: COMPLETE`; reaching guest code
alone is not success.

## Checkpoint and restore

```sh
tests/gem5/vmx/run.sh checkpoint
tests/gem5/vmx/run.sh restore
```

The checkpoint is taken while VMX root operation is active and a current VMCS
contains a known guest RIP.  Restore rechecks the capability contract,
VMPTRST, and VMREAD before VMXOFF.  The default checkpoint is
`m5out-vmx-checkpoint/cpt-vmx-active`; set `VMX_CHECKPOINT_DIR` to use another
absolute path.

## Build and static verification

```sh
CC=gcc-14 CXX=g++-14 scons build/X86/gem5.fast -j"$(nproc)"
git diff --check
sh -n tests/gem5/vmx/run.sh
python3 -m py_compile tests/gem5/vmx/config.py
python3 util/style.py --modifications
python3 util/style.py src/arch/x86/vmcs.test.cc \
    src/arch/x86/vmx_utils.hh src/arch/x86/vmx_utils.test.cc
```

The supplied Linux 5.4.49 kernel enables module symbol versioning.  The test
Makefile rebuilds each module and patches every generated symbol-version CRC
from the exact supplied unstripped kernel before relinking, rather than using
only the source tree's `Module.symvers`.  It uses Linux's CR4 shadow-aware
helpers when enabling VMXE so ordinary kernel TLB maintenance cannot
accidentally clear VMXE while VMX operation is active.

## Differential testing

The modules use ordinary Intel VMX instructions and can be adapted to a real
Intel machine, but the transition test's gem5 resource/mapping harness is not
an automated bare-metal differential runner.  A differential result should
record CPU model plus every relevant VMX capability MSR, then compare status
flags, VM-instruction error, exit reason/qualification, saved guest RIP,
instruction length, and relevant VMCS fields.  Timing is not comparable.

Real-hardware differential runs, async interrupt/NMI behavior, compatibility
mode, non-root checkpointing, and the full byte/fault-priority matrix remain
outside the automated coverage.  See `docs/vmx-architectural-audit.md` before
using the model for research.
