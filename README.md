# gem5 Intel VMX Support

This branch extends gem5's x86 ISA model with a functional subset of Intel
Virtual Machine Extensions (VMX). It supports the complete basic lifecycle
needed to enter VMX operation, launch a small 64-bit guest, handle selected VM
exits, resume the guest, and return to normal execution.

The implementation follows the architectural behavior described in the
[Intel 64 and IA-32 Software Developer's Manual, Volume
3C](https://www.intel.com/content/www/us/en/content-details/868148/intel-64-and-ia-32-architectures-software-developer-s-manual-volume-3c-system-programming-guide-part-3.html).
It is still a deliberately limited VMX model, not a complete replacement for
every hardware virtualization facility provided by a current Intel processor.

## Implemented VMX instructions

The following instructions have functional behavior in VMX root and non-root
operation:

- `VMXON` and `VMXOFF`
- `VMCLEAR`, `VMPTRLD`, and `VMPTRST`
- `VMREAD` and `VMWRITE`
- `VMLAUNCH` and `VMRESUME`
- `VMCALL`

The implementation checks instruction recognition, privilege level, VMX root
or non-root state, `IA32_FEATURE_CONTROL`, `CR4.VMXE`, fixed CR0/CR4 bits,
physical-address validity, VMX-region alignment, and VMCS revision identifiers.
It reports success, VMfailInvalid, VMfailValid, `#UD`, and `#GP` through the
architectural paths and flag values defined by Intel.

VMX instructions executed in non-root operation cause the appropriate VM exit
instead of being rejected by an early decode-time CPL check.

## VMCS and capability support

VMCS regions are associated with typed VMCS state while retaining their
architectural physical addresses. The model tracks the current VMCS pointer,
clear and launched state, VM-instruction errors, entry and exit information,
and supported control and state fields. VMX state is also included in gem5
checkpoint serialization.

The x86 capability MSRs are intentionally conservative: an allowed control bit
is a promise that the corresponding behavior is implemented and tested. This
branch advertises:

- VMCS revision, 4 KiB region size, write-back memory type, and true controls
- CR0 and CR4 fixed-bit requirements for the supported paged, 64-bit VMX mode
- CR3-load exiting as the only optional primary processor control; CR3 targets
  are unavailable
- 64-bit host operation and guest/host `IA32_EFER` transitions

All pin-based controls are required to be zero. The model therefore does not
advertise external-interrupt or NMI exiting. It also does not advertise HLT,
INVLPG, MOV-DR, RDTSC, or CR8 access exiting; unconditional-I/O exiting or I/O
bitmaps; or MSR bitmaps. Some of those paths have internal hooks for future
work, but they are not part of the supported VMX interface.

Secondary controls, EPT, VPID, VMFUNC, and related capability MSRs are also
unavailable. The complete capability and verification matrix is in
[the VMX architectural audit](docs/vmx-architectural-audit.md).

## VM entry and exit

`VMLAUNCH` and `VMRESUME` validate the requested controls and VMCS state before
entering non-root operation. Validation covers control capabilities, CR0/CR4
fixed bits, canonical addresses, segment attributes, descriptor tables, task
state, activity and interruptibility state, and the relationships between
paging, IA-32e mode, and `IA32_EFER`.

Successful VM entry loads guest control registers, segment state, descriptor
tables, task and local-descriptor state, debug state, `RSP`, `RIP`, `RFLAGS`,
and `IA32_EFER` from the VMCS.

On a VM exit, gem5 records the architectural reason, qualification,
instruction length, and guest linear address when applicable. Guest state is
saved to the VMCS before host control, segment, descriptor-table, task, stack,
instruction-pointer, debug, and EFER state is loaded from the host-state area.
The host restore no longer depends on a simulator-only snapshot taken before
VM entry.

The supported exit contract includes `VMCALL`, CR0/CR4 access exits governed by
guest/host masks, configured CR3-load exits, exception-bitmap exits, and
unconditional non-root exits for `CPUID` and VMX instructions. `RDMSR` and
`WRMSR` have a partial unconditional-exit path in non-root operation, but MSR
bitmap filtering and an end-to-end MSR-exit regression are unavailable. The
remaining interception hooks are deliberately not advertised as supported
controls; see the architectural audit for their status and test coverage.

The `VMX` debug flag prints concise traces of VMXON, VMCS selection, entry
validation, guest and host state transitions, exit reasons, and VMXOFF:

```sh
build/X86/gem5.opt --debug-flags=VMX --debug-file=vmx.trace \
    <full-system-config.py> <config arguments>
```

## Related x86 instruction support

The VMX work also fills in four baseline x86 instructions that a normal VMM
uses while collecting host state. They are not VMX controls themselves, but a
VMM needs their results to populate the descriptor-table and task-state parts
of a VMCS without relying on gem5-specific shortcuts.

- `SGDT` stores the current global-descriptor-table limit and base to memory.
- `SIDT` does the same for the interrupt-descriptor table.
- `SLDT` returns the local-descriptor-table selector to a general-purpose
  register or memory.
- `STR` returns the current task-register selector to a general-purpose
  register or memory.

Before this work, the decoder selected legacy helper stubs for these store
forms, but the corresponding executable microcode was absent. The decoder now
selects concrete instruction macro-operations. `SGDT` and `SIDT` read the
architectural limit and base and store the packed pseudo-descriptor; `SLDT` and
`STR` read the architectural selector and store or return its 16-bit value.
Each instruction has ordinary memory and RIP-relative variants, and `SLDT` and
`STR` also have register forms. The micro-operations are serializing and use
gem5's normal address-generation and segment handling, so VMM code can use
ordinary x86 instruction encodings rather than simulator-only access paths.

## Building

Build the x86 optimized binary in the normal gem5 way:

```sh
scons build/X86/gem5.opt -j"$(nproc)"
```

VMX testing requires full-system mode because VMX instructions are privileged.
Use an out-of-tree loadable kernel module built against the guest's exact
kernel. The module should allocate ordinary VMXON and VMCS pages, configure a
minimal VMCS, launch a guest payload, check every VM-exit reason, execute
VMXOFF, restore CR4, and free all allocated memory. Test modules, kernel
built-in hooks, generated objects, and test-only disk images should not be
added to this repository.

The end-to-end lifecycle exercised during development is:

```text
VMXON -> VMCLEAR -> VMPTRLD -> VMWRITE/VMREAD -> VMLAUNCH
      -> VMCALL exit (18) -> VMRESUME -> VMCALL exit (18) -> VMXOFF
```

This sequence has been validated with a Linux 5.4.49 external module in the
standard gem5 Ubuntu 18.04 full-system image. A normal x86 syscall-emulation
smoke test is also used to check that the decoder changes do not regress
non-VMX execution.

## Current limitations

The following facilities are not implemented and are not advertised:

- Extended Page Tables (EPT)
- Virtual Processor Identifiers (VPID)
- external-interrupt, NMI, and interrupt-window exiting
- HLT, INVLPG, MOV-DR, RDTSC, and CR8 access exiting
- unconditional-I/O exiting, I/O bitmaps, and MSR bitmaps
- unrestricted guests and real-mode guests
- APIC virtualization, virtual interrupts, and posted interrupts
- VM-entry event injection
- VM-entry and VM-exit MSR load/store lists
- VM functions, the VMX preemption timer, and SMM dual-monitor treatment
- running an unmodified nested Linux or general-purpose hypervisor guest

The current goal is an accurate, inspectable implementation of the supported
VMX lifecycle. Additional controls should be advertised only when their state
validation, execution behavior, exit information, and checkpoint behavior are
implemented together.
