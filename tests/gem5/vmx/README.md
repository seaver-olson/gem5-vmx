# VMX full-system smoke test

This test exercises VMX from CPL0 in the supplied Linux 5.4.49 guest. It checks
CPUID and VMX capability MSRs, then executes:

```text
VMXON -> VMCLEAR -> VMPTRLD -> VMPTRST
      -> VMWRITE/VMREAD round-trip -> VMXOFF -> PASS
```

The module stays in VMX root operation between `insmod` and `rmmod`. The
checkpoint mode asks gem5 to save a checkpoint in that interval, so the
checkpoint contains a live VMXON pointer, current VMCS pointer, and shadow VMCS
field map. The restore run verifies those values again before `VMXOFF`.

## Quick run

From the repository root:

```sh
tests/gem5/vmx/run.sh
```

The runner builds `vmx_smoke.ko` against `resources/linux-5.4.49`, embeds it in
the guest readfile, boots the local x86 kernel/disk resources with KVM, switches
to gem5's Atomic CPU immediately before the VMX instructions, and checks for a
`vmx_smoke: PASS: COMPLETE` line. VMX therefore runs in the simulator, not on
the host CPU. Override the gem5 binary with `GEM5=/path/to/gem5.opt`.

The runner prints the result of every instruction, including the values used
for the VMCS pointer and `GUEST_RIP` round trip:

```text
VMX instruction results:
  [PASS] honest capability and read-only MSR contract
  [PASS] VMXON
  [PASS] VMCLEAR ignores revision identifier
  [PASS] VMPTRLD
  [PASS] VMPTRLD rejects bad revision with error 11
  [PASS] VMPTRST (current VMCS=...)
  [PASS] VMWRITE GUEST_RIP=0x123456789abcdef0
  [PASS] VMREAD GUEST_RIP=0x123456789abcdef0
  [PASS] VMPTRST state verification
  [PASS] VMREAD state verification
  [PASS] VMXOFF
  [PASS] COMPLETE (all state checks passed)
VMX smoke: PASS (normal)
```

KVM is the fast default. On a machine without `/dev/kvm`, use the slower
all-Atomic path:

```sh
VMX_NO_KVM=1 tests/gem5/vmx/run.sh
```
The supplied Linux 5.4 tree enables module symbol versioning. The Makefile
uses GCC 13 and a frame-pointer unwinder to avoid Linux 5.4 incompatibilities
with newer host toolchains. It then reads every referenced symbol CRC from the
exact supplied unstripped kernel, patches the generated module-version table,
and relinks. The result therefore matches the prebuilt kernel rather than only
the source tree's `Module.symvers`.

## Checkpoint and restore

```sh
tests/gem5/vmx/run.sh checkpoint
tests/gem5/vmx/run.sh restore
```

The default checkpoint is
`m5out-vmx-checkpoint/cpt-vmx-active`. Override it with
`VMX_CHECKPOINT_DIR=/absolute/path` for both commands.

## Architectural transition regression

```sh
tests/gem5/vmx/run.sh transition
```

The transition module builds two guest page-table roots and gives the host,
guest root A, and guest root B different physical mappings for the same linear
address. It verifies real instruction/data translation after VMLAUNCH, a
non-exiting guest CR3 write, a configured CR3-load exit, and immediate host
address-space restoration. It also covers VMCALL/VMRESUME, guest RSP, CR0
mask/read-shadow and CLTS behavior, direct #PF metadata and CR2 preservation,
VMCLEAR launch lifecycle, late-entry-failure field/clear-state preservation,
and pre-commit failure atomicity.

The normal/checkpoint modes remain deliberately fast root-operation tests.
The transition mode is the maintained non-root regression for the supported
single-vCPU, IA-32e foundational subset; it is not an EPT or nested-OS test.
