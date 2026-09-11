# VMX full-system smoke test

This test exercises VMX from CPL0 in the supplied Linux 5.4.49 guest. It checks
CPUID and VMX capability MSRs, then executes:

An automated, best-effort run of this suite is also wired up as the
`vmx-full-system` job in `.github/workflows/vmx-tests.yaml` (weekly schedule
and manual dispatch). The `vmx-unit-tests` job in the same workflow runs the
pure-logic gtest suite (`src/arch/x86/vmx_utils.test.cc`,
`src/arch/x86/vmcs.test.cc`) on every push/PR that touches VMX code -- that
tier needs no kernel, no KVM, and no network resources, so it is the reliable,
always-on signal; treat the full-system job as a slower regression tier.

```text
VMXON -> VMCLEAR -> VMPTRLD -> VMPTRST
      -> VMWRITE/VMREAD round-trip -> VMXOFF -> PASS
```

The module stays in VMX root operation between `insmod` and `rmmod`. The
checkpoint mode asks gem5 to save a checkpoint in that interval, so the
checkpoint contains a live VMXON pointer, current VMCS pointer, and shadow VMCS
field map. The restore run verifies those values again before `VMXOFF`.

## Prerequisites: bootstrapping `resources/`

Nothing here is fetched automatically the first time. Before the first run
(or after deleting `resources/`, which is gitignored):

```sh
# 1. The published gem5 kernel image + disk image (uses gem5's own
#    resource-fetching library, so it must run through gem5's Python, not
#    the system interpreter).
build/X86/gem5.opt tests/gem5/vmx/fetch_resources.py

# 2. The matching Linux 5.4.49 *source* tree, to build the .ko against.
curl -sL -o /tmp/linux-5.4.49.tar.xz \
    https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.4.49.tar.xz
tar -xf /tmp/linux-5.4.49.tar.xz -C resources/
```

A vanilla `defconfig` for that source tree defaults to `CONFIG_MODULES=n`
(can't build a `.ko` at all) and only populates `Module.symvers` with real
CRCs the first time it's built *with* `CONFIG_MODULES`/`CONFIG_MODVERSIONS`
already on -- so those must be set before that first build, not patched in
afterward once `Module.symvers` already exists (empty). The exact sequence
that works (also encoded in `.github/workflows/vmx-tests.yaml`):

```sh
KDIR=resources/linux-5.4.49
make -C "$KDIR" CC=gcc-13 HOSTCC=gcc-13 LOCALVERSION= \
    SKIP_STACK_VALIDATION=1 defconfig
"$KDIR"/scripts/config --file "$KDIR"/.config \
    --enable MODULES --enable MODVERSIONS --enable MODULE_SRCVERSION_ALL \
    --disable MODULE_SIG --disable RANDOMIZE_BASE --disable RANDOMIZE_MEMORY \
    --disable NUMA --disable UNWINDER_ORC --enable UNWINDER_FRAME_POINTER
make -C "$KDIR" CC=gcc-13 HOSTCC=gcc-13 LOCALVERSION= \
    SKIP_STACK_VALIDATION=1 olddefconfig
WERROR=0 make -C "$KDIR" CC=gcc-13 HOSTCC=gcc-13 LOCALVERSION= \
    SKIP_STACK_VALIDATION=1 -j"$(nproc)"
```

`RANDOMIZE_MEMORY`/`NUMA` are disabled because the published
`x86-linux-kernel-5.4.49-1.0.0` image was not built with them: a module
built against them references symbols (`page_offset_base`, `vmemmap_base`,
`alloc_pages_current`) that simply don't exist in that image, which the
CRC-patching step below cannot paper over (it now fails loudly and names the
missing symbol, rather than silently corrupting the generated
`*.mod.c` -- see "CRC patching" below). `WERROR=0` works around Linux 5.4's
bundled `tools/objtool` failing `-Werror=use-after-free` under GCC 13+, a
GCC/kernel-vintage mismatch rather than a gem5-vmx bug.

None of this is a substitute for the *original* `.config` used to build
`x86-linux-kernel-5.4.49-1.0.0` -- it is a reconstruction that gets far
enough to build and CRC-match the module. If future module-load failures
appear, compare the module's `vermagic` and referenced symbol CRCs against
the supplied kernel before changing VMX code.

## Quick run

From the repository root, once `resources/` is bootstrapped as above:

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
The Linux 5.4 tree enables module symbol versioning. The Makefile uses GCC 13
and a frame-pointer unwinder to avoid Linux 5.4 incompatibilities with newer
host toolchains. It then reads every referenced symbol CRC from the exact
supplied unstripped kernel, patches the generated module-version table, and
relinks. The result therefore matches the prebuilt kernel rather than only
the source tree's `Module.symvers`. If a referenced symbol's CRC can't be
found in that kernel at all, the Makefile now fails immediately with an
`ERROR: ... has no __crc_<symbol>` message naming it, instead of silently
leaving a malformed placeholder that only surfaces later as a confusing C
compile error in the generated `*.mod.c` (a real, if latent, bug in the
original CRC-patching loop: it piped `sed | while read ...; do ...; done`
without `pipefail`, so a failed `test -n "$crc"` inside the loop only ended
that subshell, never the `make` recipe).

## Validation status

`tests/gem5/vmx/run.sh transition` has been verified with KVM fast-forward
and the Atomic CPU switch. The run loads `vmx_transition.ko`, executes the
VMX transition sequence, and reports `VMX transition: PASS (transition)`.
On systems without `/dev/kvm`, use `VMX_NO_KVM=1`; that path boots entirely
on Atomic and is much slower.

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
GETSEC/XSETBV pre-exit #UD priority, VMCLEAR launch lifecycle,
late-entry-failure field/clear-state preservation, and pre-commit failure
atomicity.

The normal/checkpoint modes remain deliberately fast root-operation tests.
The transition mode is the maintained non-root regression for the supported
single-vCPU, IA-32e foundational subset; it is not an EPT or nested-OS test.
