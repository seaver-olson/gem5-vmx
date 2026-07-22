# gem5-vmx implementation contract

## Intended use

This branch implements a deliberately narrow Intel VMX foundation in gem5's
x86 ISA.  Its verified target is a controlled, single-logical-processor,
64-bit host and 64-bit IA-32e guest using ordinary x86 paging.  It models VMX
root/non-root state, a typed implementation-specific VMCS, VM entry and exit,
selected interception, architectural instruction results, and checkpoint
state.

It is not a nested-virtualization stack.  EPT, VPID, unrestricted guest, APIC
virtualization, event injection, MSR lists, VMCS shadowing, nested VMX, and
multi-vCPU VMCS ownership are not advertised.  The authoritative feature
matrix, findings, Intel references, and verification results are in
`docs/vmx-architectural-audit.md`.

## Code organization

| Path | Responsibility |
|---|---|
| `src/arch/x86/isa/decoder/two_byte_opcodes.isa` | Exact VMX opcode/mandatory-prefix selection, address-size propagation, and ordinary non-root instruction exits. |
| `src/arch/x86/isa/formats/vmx.isa` | VMX static instructions, register/memory forms, effective addresses, operand faults, status flags, and control redirection. |
| `src/arch/x86/insts/vmx.hh`, `vmx.cc` | Per-CPU VMX state, instruction semantics, validation, ordered entry/exit, state transfer, intercept decisions, and serialization. |
| `src/arch/x86/vmcs.hh` | VMCS encodings, metadata, widths, high/full access, read-only rules, typed storage, and lifecycle state. |
| `src/arch/x86/vmx_utils.hh` | Shared capability, fixed-bit, CR3, CR0-load, CLTS, and LMSW rules. |
| `src/arch/x86/isa.cc` | Capability/fixed MSR reset contract, ordinary register side effects, CPU-model switching, and VMX serialization ownership. |
| `src/arch/x86/isa/insts/system/control_registers.py` and `isa/microops/regop.isa` | Shared MOV-CR/CLTS/LMSW virtualization and root fixed-bit enforcement. |
| `src/arch/x86/faults.cc` | Common synchronous-exception and selected event VM-exit path. |
| `src/arch/x86/tlb.cc`, `regs/msr.cc`, `isa/insts/system/msrs.py` | VMX MSR mapping, read-only behavior, `IA32_FEATURE_CONTROL`, and MSR exits. |
| `src/arch/x86/SConscript`, `vmcs.test.cc`, `vmx_utils.test.cc` | Unit-test build targets and metadata/validation regressions. |
| `tests/gem5/vmx` | CPL0 smoke, real page-table transition, negative entry, and checkpoint/restore tests. |

## VMX and VMCS state

Each x86 ISA context owns one `VmxState` containing:

- VMX-operation and VMX-non-root flags;
- the VMXON-region physical address;
- the current VMCS pointer; and
- typed VMCS objects keyed by physical region address.

Each VMCS has explicit clear/launched and active state.  `VMCLEAR` preserves
implementation-specific fields while returning the VMCS to clear/inactive
state; Intel does not require it to inspect the revision identifier.
`VMPTRLD` performs the revision/shadow checks, makes the VMCS current, and
marks it active.  `VMXOFF` clears current/active state.  Active ownership by a
different logical processor is not represented, so the verified system must
contain one logical processor.

The architectural VMCS region is still a physical memory region with a
revision/shadow/abort header.  Intel does not define an in-memory layout for
the remaining implementation-specific data, so guest software accesses typed
fields only with VMREAD/VMWRITE.  Unknown, reserved, read-only, or unsupported
encodings fail architecturally; they do not become arbitrary map entries.

VMCS metadata implements 16-, 32-, 64-, and natural-width components,
including high-half access to 64-bit fields.  Writes truncate to field width;
high writes preserve the low half.  Natural-width behavior follows the current
execution mode.  Exit-information fields are writable internally by the
simulator but read-only to VMWRITE.

## VMX-instruction results

The implemented instructions are VMXON, VMXOFF, VMCLEAR, VMPTRLD, VMPTRST,
VMREAD, VMWRITE, VMLAUNCH, VMRESUME, and VMCALL.

Instruction-specific checks preserve Intel's outcome classes:

- recognition/mode/CR4.VMXE failures produce `#UD` where specified;
- CPL and feature-control failures produce `#GP`;
- VMfailInvalid sets CF when no current VMCS can receive an error;
- VMfailValid sets ZF and writes the VM-instruction error;
- success clears CF and ZF; and
- other RFLAGS bits are preserved.

Memory forms carry gem5's decoded address-size and segment request flags into
the MMU.  The whole operand range is checked for canonicality; a stack-segment
violation produces `#SS`, otherwise `#GP`.  Memory access uses the normal
gem5 fault path.  VMWRITE performs architecturally earlier field checks before
reading a memory source where Intel's priority requires that ordering.

## Capability reporting

CPUID leaf 1 reports VMX.  `IA32_FEATURE_CONTROL` resets locked with VMXON
outside SMX enabled and is read-only.  `IA32_VMX_BASIC` reports revision 1, a
4096-byte WB region, and true controls.

The only optional primary processor control is CR3-load exiting.  Pin controls
are all unavailable.  VM exit requires a 64-bit host and optionally permits
save/load EFER.  VM entry requires an IA-32e guest and optionally permits load
EFER.  True and legacy control MSRs agree.  VMCS enumeration is 0x2a.  PCIDE
is excluded by CR4 fixed bits.  Secondary/tertiary, EPT/VPID, VMFUNC, and
secondary-exit capability MSRs are unmapped, so RDMSR faults instead of
returning a permissive mask.

Capability MSRs describe CPU-model identity rather than mutable architectural
state.  CPU takeover therefore preserves the destination ISA's initialized
capabilities while copying dynamic register and VMX state.  This prevents KVM
fast-forward from leaking the physical host's VMX controls into Atomic or
detailed gem5 execution.

## VM entry

VMLAUNCH requires a clear current VMCS; VMRESUME requires a launched current
VMCS.  Entry is split into explicit phases:

1. instruction and launch-state preconditions;
2. control-field validation against the capability MSRs;
3. host-state validation;
4. guest-state validation for the accepted subset;
5. a commit boundary separating VMfailValid from late entry failure;
6. guest EFER, CR4, CR0, and CR3 loading through x86 architectural interfaces;
7. segment selectors and hidden caches, GDTR/IDTR, SYSENTER, DR7, RSP,
   RFLAGS, and RIP loading;
8. decoder/MMU mode synchronization, TLB transition, and non-root entry; and
9. launch-state update only for a committed VMLAUNCH.

Validation covers required fields, capability masks, zero unsupported counts
and injection state, CR fixed bits, CR3 reserved/physical bits, EFER/mode
relationships, host selectors and canonical pointers, guest RFLAGS/activity/
interruptibility/link state, and segment selector/base/limit/access-right
rules.  The accepted contract requires a paged protected IA-32e guest; legacy
PAE is rejected because PDPTE validation/loading is not implemented.

An early failure returns in root operation without installing guest state or
launching the VMCS.  A late invalid-guest-state failure loads host state but
also leaves the VMCS clear because VMLAUNCH changes launch state only after
guest-state and MSR loading succeed.  It sets the entry-failure exit reason
and zero qualification and leaves unrelated exit-information fields unchanged.

## VM exit

The common exit path performs these operations in order:

1. save guest control/debug, segment/cache, table, SYSENTER, EFER-selected,
   and RIP/RSP/RFLAGS state;
2. write only metadata defined for the exit source;
3. load host EFER, CR4, CR0, CR3, segment/cache, table, SYSENTER, and stack
   state through gem5 architectural interfaces;
4. update derived mode/decoder state and invalidate the no-VPID translation
   context;
5. leave non-root operation; and
6. redirect to host RIP in the restored host address space.

VMCALL, all VMX instructions, and CPUID exit unconditionally in non-root
operation.  CR0/CR4 mask/shadow behavior and CR3-load exiting use shared CR
micro-ops.  Directly intercepted page faults report the faulting linear
address in exit qualification and do not update CR2.  With MSR bitmaps
unavailable, non-root RDMSR/WRMSR use their unconditional-exit path.  Optional
HLT, INVLPG, MOV-DR, CR8, I/O, RDTSC, and pin-based exits are not enabled by
the capability contract.  INVD, GETSEC, and XSETBV take their distinct
unconditional exits in non-root operation, but their root-operation behavior
remains baseline gem5's warning/no-op implementation and is outside this
contract.

## CR3 and address translation

Entry, guest MOV-to-CR3, and exit all use
`ThreadContext::setMiscReg(misc_reg::Cr3, value)`.  This is gem5's normal x86
mechanism: it updates the architectural root consumed by the page-table
walker and invalidates non-global translations.  VM entry/exit also flush the
modeled TLB because VPID is not available and host and guest both use VPID 0.
CR0, CR4, EFER, segment attributes, and derived `M5Reg` are changed in a
consistent order so decoder and MMU mode agree before instruction fetch.

The transition regression maps one linear address to separate host, guest-A,
and guest-B physical pages.  It proves data from all three contexts, including
a non-exiting guest CR3 switch, fault-like CR3-load exit, and the host mapping
at the first exit-handler code.  PCID, legacy PAE/PDPTE, VPID, and EPT are not
part of this conclusion.

## Checkpoints and CPU switching

ISA checkpoints contain dynamic VMX flags/pointers plus every typed VMCS,
field, active flag, and launch state.  Restore rejects invalid counts,
unsupported fields, bad launch encodings, and inconsistent current/active
state.  The full-system test checkpoints while VMX root operation and a
current VMCS are live, restores it, rechecks VMPTRST/VMREAD, and executes
VMXOFF.  Checkpointing while executing in non-root operation is serialized but
not regression-tested and is outside the verified claim.

CPU-model takeover copies dynamic `VmxState` but not destination model
capability MSRs.  The full-system tests fast-forward with KVM, switch to
Atomic, then verify the narrow capability contract and execute VMX in gem5.

## Unsupported and research limits

The implementation intentionally excludes:

- multiple logical processors and cross-CPU active-VMCS ownership;
- legacy/real/unrestricted guests, legacy PAE PDPTEs, and PCID;
- EPT, VPID, APIC virtualization, posted interrupts, nested VMX, VMFUNC, and
  VMCS shadowing;
- event injection, MSR load/store lists, I/O/MSR bitmaps, CR3 targets, TSC
  virtualization, interrupt/NMI-window exits, and preemption timer;
- complete asynchronous interruptibility (`STI`/`MOV SS` shadow), the
  VM-entry precondition while blocked by `MOV SS`, triple fault/shutdown, VMX
  abort, and physical-access machine-check behavior;
- architectural root-operation INVD, GETSEC, and XSETBV behavior; and
- real-hardware differential coverage and non-root checkpoint testing.

Use this model only when those exclusions do not affect the research question
and keep the regression suite attached to published experiments.
