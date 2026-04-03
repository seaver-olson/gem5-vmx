# VMX Phased Task List Tied To gem5 Files

## Purpose

This document converts the higher-level VMX support outline into a concrete,
phased implementation plan tied to specific files in this gem5 tree.

It is intentionally pragmatic:

- each phase has a narrow goal
- each task names the likely files to edit
- each phase defines an exit criterion
- scope is biased toward landing a first working VMX path before broader
  architectural coverage

This plan assumes the first target is x86-64 VMX without nested virtualization
or EPT in the initial milestone set.

## Reading This Plan

The file lists below use these meanings:

- primary files: expected to carry the core implementation
- secondary files: likely supporting edits
- possible new files: files that probably should be introduced instead of
  overloading existing headers

Not every listed file will necessarily need changes, but these are the main
places the work is likely to land in this tree.

## Phase 0: Freeze Scope And Architectural Contract

### Goal

Decide exactly what "VMX support" means for v1 so implementation can proceed
without constant scope churn.

### Primary files

- `docs/vmx_support_outline.md`
- `docs/vmx_phased_task_list.md`

### Secondary files

- `src/arch/x86/regs/msr.hh`
- `src/arch/x86/regs/msr.cc`
- `src/arch/x86/regs/misc.hh`

### Tasks

- Define the supported VMX feature subset for v1.
- Decide whether v1 supports only long mode guests or also legacy modes.
- Decide whether unrestricted guest is in or out for v1.
- Decide whether `VMCALL` is required in v1.
- Decide which VM exits are mandatory in v1.
- Decide which VMX MSRs must be exposed accurately enough for the target
  software.
- Define a supported VMCS field list for v1 rather than trying to implement the
  whole SDM at once.

### Deliverables

- Written feature matrix in docs.
- Written "unsupported in v1" list in docs.
- Written milestone success criteria.

### Exit criterion

The team can answer "what exact VMX subset are we implementing first?" in one
page without ambiguity.

## Phase 1: Add Core VMX Architectural State

### Goal

Create per-thread or per-core VMX architectural state that can represent VMX
root operation, VMXON state, current VMCS pointer, and VMCS lifecycle.

### Primary files

- `src/arch/x86/vmcs.hh`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`

### Secondary files

- `src/arch/x86/types.hh`
- `src/arch/x86/types.cc`
- `src/cpu/exec_context.hh`

### Possible new files

- `src/arch/x86/vmx.hh`
- `src/arch/x86/vmx.cc`

### Tasks

- Add a VMX architectural state container owned by the x86 ISA state.
- Track whether the CPU is in VMX operation.
- Track VMXON region physical address.
- Track current VMCS physical address.
- Track whether a VMCS is active, current, clear, or launched on this CPU.
- Decide whether VMCS objects are globally indexed by physical address or
  attached only to the active CPU.
- Add helper methods for VMX state reset on CPU reset and checkpoint restore if
  needed.

### Notes on file ownership

- `src/arch/x86/vmcs.hh` should own VMCS representation and local lifecycle
  helpers.
- `src/arch/x86/isa.hh` and `src/arch/x86/isa.cc` are the best current homes
  for per-ISA architectural state unless a separate VMX state file is added.

### Exit criterion

The ISA model can represent VMXON state and current-VMCS state without using
ad hoc booleans spread across unrelated files.

## Phase 2: Model VMX MSRs And Capability Surface

### Goal

Expose the minimum set of VMX-related MSRs and capability bits needed for real
software to initialize VMX sanely.

### Primary files

- `src/arch/x86/regs/msr.hh`
- `src/arch/x86/regs/msr.cc`
- `src/arch/x86/regs/misc.hh`

### Secondary files

- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/cpuid.hh`
- `src/arch/x86/cpuid.cc`

### Tasks

- Add missing VMX-related MSR definitions.
- Define returned values for `IA32_VMX_BASIC`.
- Define policy for `IA32_FEATURE_CONTROL`.
- Define the fixed-bit behavior for `CR0` and `CR4` VMX-related constraints.
- Define control capability MSR values consistent with the intended v1 feature
  set.
- Ensure `CPUID` exposes VMX capability consistently with MSR behavior.

### Risks

- If CPUID claims VMX but MSRs are incomplete or contradictory, guest software
  will fail during setup.
- If fixed-bit MSRs are wrong, `VMXON` enable paths will misbehave.

### Exit criterion

Guest software can probe VMX capability and receive a coherent set of answers.

## Phase 3: Build A Real VMCS Field Model

### Goal

Replace the current lightweight VMCS scaffold with a field-encoding-based VMCS
storage layer.

### Primary files

- `src/arch/x86/vmcs.hh`

### Possible new files

- `src/arch/x86/vmcs.cc`
- `src/arch/x86/vmcs_fields.hh`
- `src/arch/x86/vmcs_fields.cc`

### Secondary files

- `src/arch/x86/types.hh`
- `src/arch/x86/types.cc`

### Tasks

- Define VMCS field encoding enums.
- Define metadata per field: width, category, writable/read-only status.
- Add storage for implemented fields.
- Add typed accessors for 16-bit, 32-bit, 64-bit, and natural-width fields.
- Add field validation helpers for `VMREAD` and `VMWRITE`.
- Add support for region header semantics such as revision ID and abort
  indicator.
- Add a coherent distinction between software bookkeeping state and the actual
  VMCS field space.

### Recommended implementation split

- `vmcs_fields.hh`: field encoding enum and metadata declarations
- `vmcs.hh`: VMCS object and access APIs
- `vmcs.cc`: field metadata tables and access logic

### Exit criterion

There is a single authoritative VMCS field model that all VMX instructions and
transition paths use.

## Phase 4: Wire VMX Instructions Into The Decoder And ISA Generator

### Goal

Replace decode-only or unimplemented VMX instruction stubs with real execution
objects.

### Primary files

- `src/arch/x86/isa/decoder/two_byte_opcodes.isa`
- `src/arch/x86/isa/main.isa`
- `src/arch/x86/isa/includes.isa`
- `src/arch/x86/isa/microasm.isa`

### Secondary files

- `src/arch/x86/isa/formats/unimp.isa`
- `src/arch/x86/isa/formats/basic.isa`
- `src/arch/x86/insts/static_inst.hh`
- `src/arch/x86/decoder.hh`
- `src/arch/x86/decoder.cc`

### Possible new files

- `src/arch/x86/isa/formats/vmx.isa`
- `src/arch/x86/isa/insts/system/vmx.py`

### Tasks

- Replace `WarnUnimpl` mappings for `VMXON`, `VMCLEAR`, `VMPTRLD`, `VMPTRST`,
  `VMREAD`, and `VMWRITE`.
- Confirm that `VMLAUNCH`, `VMRESUME`, `VMXOFF`, and `VMCALL` have real
  execution bodies, not just decode entries.
- Introduce a VMX instruction format if the current ISA generator structure
  makes that cleaner than embedding logic elsewhere.
- Ensure memory operand decoding rules match VMX opcode requirements.
- Ensure mode restrictions produce the right exception or VMfail path.

### Exit criterion

All first-cut VMX instructions decode into real implementations.

## Phase 5: Implement VMX Instruction Preconditions And Error Reporting

### Goal

Make each VMX instruction perform the correct privilege checks, mode checks,
operand checks, and success/failure reporting.

### Primary files

- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`
- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/vmcs.hh`

### Secondary files

- `src/arch/x86/regs/misc.hh`
- `src/arch/x86/regs/msr.hh`
- `src/arch/x86/regs/msr.cc`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`

### Possible new files

- `src/arch/x86/vmx.hh`
- `src/arch/x86/vmx.cc`

### Tasks

- Add shared helpers for VMX instruction legality checks.
- Implement `CR4.VMXE` handling.
- Implement VMX root-operation preconditions.
- Implement alignment and physical-address checks for VMX memory operands.
- Define how VMfail invalid and VMfail valid are represented in gem5.
- Ensure status flags are updated properly on VMX instruction completion.
- Add or extend fault classes if existing x86 faults do not cleanly represent
  VMX instruction behavior.

### Exit criterion

Each VMX instruction can fail in a controlled architectural way instead of
silently mutating state or falling back to a generic fault.

## Phase 6: Implement VMXON, VMXOFF, VMCLEAR, VMPTRLD, VMPTRST

### Goal

Bring up the VMX management instruction set before attempting guest entry.

### Primary files

- `src/arch/x86/vmcs.hh`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/isa/decoder/two_byte_opcodes.isa`

### Secondary files

- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`

### Tasks

- Implement `VMXON` region validation and state entry.
- Implement `VMXOFF` state teardown.
- Implement `VMCLEAR` state transitions and launch-state reset.
- Implement `VMPTRLD` current-VMCS selection.
- Implement `VMPTRST` reporting of the current VMCS pointer.
- Ensure instruction ordering and error cases match the chosen VMX subset.

### Exit criterion

A directed test can enter VMX operation, create/select a VMCS, clear it, and
leave VMX operation without using any unimplemented instruction path.

## Phase 7: Implement VMREAD And VMWRITE

### Goal

Enable software to populate and inspect the VMCS through architectural access
instructions.

### Primary files

- `src/arch/x86/vmcs.hh`
- `src/arch/x86/vmcs.cc` if added
- `src/arch/x86/isa/decoder/two_byte_opcodes.isa`

### Secondary files

- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`

### Tasks

- Implement field encoding decode.
- Implement width-aware reads and writes.
- Reject invalid or unsupported fields cleanly.
- Enforce read-only versus writable field policy.
- Handle memory and register forms correctly.

### Exit criterion

A directed test can populate the supported VMCS fields entirely through
`VMWRITE` and read them back with `VMREAD`.

## Phase 8: Add Guest And Host State Load/Store Helpers

### Goal

Create reusable host/guest state transfer helpers used by `VMLAUNCH`,
`VMRESUME`, and VM exits.

### Primary files

- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/regs/segment.hh`
- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/vmcs.hh`

### Secondary files

- `src/arch/x86/pcstate.hh`
- `src/arch/x86/regs/misc.hh`
- `src/arch/x86/regs/int.hh`
- `src/arch/x86/regs/int.cc`

### Possible new files

- `src/arch/x86/vmx_state.hh`
- `src/arch/x86/vmx_state.cc`

### Tasks

- Add helpers to read guest state from VMCS into architectural registers.
- Add helpers to write guest state back into the VMCS on exit.
- Add helpers to load host state from VMCS host fields.
- Add helpers for RIP, RSP, RFLAGS, CR0, CR3, CR4, segment registers, GDTR,
  IDTR, TR, and LDTR as required by the chosen scope.
- Define which state is intentionally unsupported in v1.

### Exit criterion

There is a single code path for guest/host state transitions instead of
instruction-local hand-written state copies.

## Phase 9: Implement VM-Entry Validation

### Goal

Reject illegal VMCS state before guest execution starts.

### Primary files

- `src/arch/x86/vmcs.hh`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`

### Secondary files

- `src/arch/x86/regs/segment.hh`
- `src/arch/x86/regs/misc.hh`
- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`

### Possible new files

- `src/arch/x86/vmx_checks.hh`
- `src/arch/x86/vmx_checks.cc`

### Tasks

- Implement guest-state validity checks.
- Implement host-state validity checks.
- Implement control-field dependency checks.
- Implement canonical-address checks for long mode.
- Implement segment and descriptor-table consistency checks.
- Implement VM-entry failure recording.
- Define which VM-instruction error codes are surfaced in v1.

### Exit criterion

`VMLAUNCH` and `VMRESUME` either enter the guest or fail for a defined reason
instead of producing undefined simulator state.

## Phase 10: Implement VMLAUNCH And VMRESUME

### Goal

Create the first real guest entry path.

### Primary files

- `src/arch/x86/isa/decoder/two_byte_opcodes.isa`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/vmcs.hh`

### Secondary files

- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`
- `src/arch/x86/pcstate.hh`

### Tasks

- Implement `VMLAUNCH` launch-state rules.
- Implement `VMRESUME` launched-state rules.
- Invoke VM-entry checks before transferring control.
- Load guest state and transfer execution to guest RIP.
- Define how non-root execution is represented in the ISA model.
- Ensure architectural flags and error codes are updated on success/failure.

### Exit criterion

A minimal VMCS can successfully enter guest execution for a directed test.

## Phase 11: Add VM-Exit Core Path

### Goal

Return from guest execution to VMX root operation with correct exit bookkeeping.

### Primary files

- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/vmcs.hh`
- `src/arch/x86/interrupts.hh`
- `src/arch/x86/interrupts.cc`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`

### Secondary files

- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/pcstate.hh`

### Possible new files

- `src/arch/x86/vmx_exit.hh`
- `src/arch/x86/vmx_exit.cc`

### Tasks

- Define a central VM-exit helper.
- Save guest architectural state into the VMCS.
- Populate exit reason and qualification fields.
- Restore host state from the VMCS host fields.
- Resume host execution at host RIP/RSP.
- Preserve launch state appropriately after exit.

### Exit criterion

A guest can execute a controlled instruction that triggers a VM exit and return
to the host correctly.

## Phase 12: Implement First Mandatory Exit Classes

### Goal

Support a narrow but useful initial set of VM exits.

### Primary files

- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`
- `src/arch/x86/interrupts.hh`
- `src/arch/x86/interrupts.cc`
- `src/arch/x86/vmcs.hh`

### Secondary files

- `src/arch/x86/isa/insts/system/control_registers.py`
- `src/arch/x86/isa/insts/system/msrs.py`
- `src/arch/x86/isa/insts/system/halt.py`
- `src/arch/x86/isa/insts/general_purpose/system_calls.py`

### Tasks

- Exit on `CPUID`.
- Exit on `HLT`.
- Exit on `VMCALL`.
- Exit on selected control-register accesses if enabled.
- Exit on selected MSR accesses if enabled.
- Capture instruction length where needed.
- Populate exit qualification for implemented exit classes.

### Exit criterion

A guest test can trigger several distinct exit reasons and the host can inspect
them from the VMCS.

## Phase 13: Add Event Injection And Interruptibility State

### Goal

Support basic interrupt and exception injection semantics needed for realistic
VM entry and exit behavior.

### Primary files

- `src/arch/x86/interrupts.hh`
- `src/arch/x86/interrupts.cc`
- `src/arch/x86/vmcs.hh`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`

### Secondary files

- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`
- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`

### Tasks

- Track guest interruptibility state.
- Support a basic event-injection path from VMCS entry fields.
- Decide how STI/MOV-SS blocking is modeled in v1.
- Handle exception injection for the limited set required by tests.
- Define which interrupts cause exits versus in-guest delivery in the initial
  design.

### Exit criterion

Directed tests can enter the guest with a basic injected event and observe
defined behavior.

## Phase 14: Decide On Memory Virtualization Strategy

### Goal

Either explicitly defer EPT or design the MMU/TLB work needed to support it.

### Primary files

- `src/arch/x86/mmu.hh`
- `src/arch/x86/tlb.hh`
- `src/arch/x86/tlb.cc`
- `src/arch/x86/pagetable.hh`
- `src/arch/x86/pagetable.cc`
- `src/arch/x86/pagetable_walker.hh`
- `src/arch/x86/pagetable_walker.cc`

### Secondary files

- `src/arch/x86/vmcs.hh`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`

### Possible new files

- `src/arch/x86/ept.hh`
- `src/arch/x86/ept.cc`

### Tasks

- For v1, document that EPT is deferred unless required immediately.
- If EPT is included, define guest-physical translation flow.
- Decide where EPT violations are generated.
- Decide how EPT interacts with TLB fill, invalidation, and page-walk logic.
- Add VMCS support for EPT pointer and EPT-related controls if EPT is enabled.

### Exit criterion

There is an explicit written decision on EPT, and the code path is consistent
with that decision.

## Phase 15: Add Diagnostics, Tracing, And Debug Support

### Goal

Make VMX failures debuggable.

### Primary files

- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/vmcs.hh`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`

### Secondary files

- `src/arch/x86/nativetrace.hh`
- `src/arch/x86/nativetrace.cc`
- `src/cpu/base.cc`

### Tasks

- Add VMCS dump helpers.
- Add VM-entry and VM-exit trace messages.
- Add debug assertions for illegal state combinations.
- Add a compact summary printer for exit reason and qualification.
- Add optional logging around VMX instruction failures.

### Exit criterion

When a VMX transition fails, the developer can determine why without extensive
manual instrumentation.

## Phase 16: KVM Backend Consistency Review

### Goal

Ensure the x86 architectural VMX implementation does not drift into obvious
conflict with the x86 KVM backend assumptions.

### Primary files

- `src/arch/x86/kvm/x86_cpu.hh`
- `src/arch/x86/kvm/x86_cpu.cc`
- `src/cpu/kvm/base.hh`
- `src/cpu/kvm/base.cc`

### Secondary files

- `src/cpu/kvm/vm.hh`
- `src/cpu/kvm/vm.cc`

### Tasks

- Review whether KVM-backed CPUs need any VMX-specific save/restore changes.
- Verify that segment normalization assumptions do not conflict with the new
  VMX architectural paths.
- Decide whether KVM CPUs should expose the same VMX capability surface or a
  constrained subset.

### Exit criterion

The architectural VMX work does not silently break KVM-backed x86 execution
paths.

## Phase 17: Tests And Documentation

### Goal

Add enough tests and docs that the implementation is usable and maintainable.

### Primary files

- `docs/vmx_support_outline.md`
- `docs/vmx_phased_task_list.md`

### Likely new test locations

- `tests/gem5/x86/` for end-to-end VMX tests
- `src/arch/x86/` unit-test-adjacent infrastructure if the tree already has a
  local testing pattern for ISA helpers

### Tasks

- Add a feature matrix documenting what VMX support exists.
- Add directed tests for each implemented VMX instruction.
- Add VM-entry success and failure tests.
- Add VM-exit reason tests for the implemented exit classes.
- Add regression tests for illegal VMCS states.
- Document unsupported controls and unsupported guest modes.

### Exit criterion

There is a repeatable directed test set that exercises the supported VMX subset
and a doc page that states the real support boundary honestly.

## Recommended Commit Sequence

If this work is done incrementally, the cleanest commit order is:

1. docs and scope freeze
2. VMX architectural state
3. VMX MSRs and CPUID exposure
4. VMCS field model
5. VMX instruction plumbing in ISA decode/generation
6. VMX management instructions
7. VMREAD and VMWRITE
8. VM-entry checks
9. VMLAUNCH and VMRESUME
10. VM-exit core path
11. initial exit classes
12. diagnostics and tests
13. optional EPT work

## Minimum Set Of Files Most Likely To Matter

If you want the shortest list of high-probability edit targets, start here:

- `src/arch/x86/vmcs.hh`
- `src/arch/x86/isa.hh`
- `src/arch/x86/isa.cc`
- `src/arch/x86/regs/msr.hh`
- `src/arch/x86/regs/msr.cc`
- `src/arch/x86/regs/misc.hh`
- `src/arch/x86/faults.hh`
- `src/arch/x86/faults.cc`
- `src/arch/x86/utility.hh`
- `src/arch/x86/utility.cc`
- `src/arch/x86/interrupts.hh`
- `src/arch/x86/interrupts.cc`
- `src/arch/x86/isa/decoder/two_byte_opcodes.isa`

## Bottom-Line Recommendation

Do not start by trying to "implement Intel VT-x" across all areas at once.

Start with:

- Phase 1 through Phase 7 to build VMX state and VMCS management
- Phase 8 through Phase 12 to get one real guest entry/exit loop working
- then expand cautiously into interrupts, broader exits, and MMU work

That sequencing gives the best chance of getting a real result without months
of churn.
