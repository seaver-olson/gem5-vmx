# VMX Support Outline For gem5 x86

## Scope

This document outlines what is required to add Intel VT-x / VMX support to
the x86 ISA model in this tree, what is already present, what is clearly
missing, and how long the work is likely to take.

This is written against the current local tree as of 2026-03-24.

## Current State In This Tree

The codebase already contains a few VMX-adjacent pieces, but it is far from a
working VT-x implementation.

Observed local evidence:

- `src/arch/x86/vmcs.hh` exists, but it is only a small software-side VMCS
  scaffold. It does not model the architectural VMCS field space.
- `src/arch/x86/regs/misc.hh` contains `CR4.VMXE`.
- `src/arch/x86/isa/decoder/two_byte_opcodes.isa` decodes `VMLAUNCH`,
  `VMRESUME`, and `VMXOFF`.
- The same decoder still routes `VMREAD`, `VMWRITE`, `VMCLEAR`, `VMXON`,
  `VMPTRLD`, and `VMPTRST` to `WarnUnimpl`.
- No local implementation was found for the actual execution semantics of
  `VMLAUNCH`, `VMRESUME`, or `VMXOFF`.
- The KVM backend contains comments about VMX-required segment properties, but
  that is host-KVM plumbing, not architectural VMX emulation.

Practical conclusion:

This tree currently has partial decode awareness and a few control bits, but it
does not have a coherent VMX architecture model. That means the work is
closer to a subsystem implementation than to a feature polish pass.

## Schedule Estimate

These estimates assume one engineer already comfortable with gem5 internals,
x86 protected mode, paging, exceptions, and the Intel SDM. They also assume
full-time work and that the goal is architectural emulation in gem5, not just
making a benchmark stop faulting.

### 1. Minimum viable VMX support

Goal:

- Enter VMX operation with `VMXON`
- Maintain VMXON and current-VMCS state
- Support `VMCLEAR`, `VMPTRLD`, `VMPTRST`, `VMREAD`, `VMWRITE`
- Support `VMLAUNCH` and `VMRESUME`
- Perform guest/host state transfer at a basic architectural level
- Produce VM exits for a narrow, explicitly limited set of conditions
- Run a tiny purpose-built VMX test payload

Estimated time:

- 6 to 10 weeks

This is only realistic if the first version intentionally limits scope:

- 64-bit only, or one carefully chosen guest mode
- no nested virtualization
- no shadow VMCS
- no EPT at first
- no unrestricted guest at first unless it materially simplifies real-mode
  handling for your test workload
- narrow exit coverage aimed at a controlled test program

### 2. Practical research-grade VMX support

Goal:

- Enough VMX behavior to run nontrivial hypervisor test code and debug VM-entry
  and VM-exit behavior with confidence
- A broad VMCS field model
- Real consistency checks
- Meaningful exit reasons and qualification data
- Stable architectural behavior under a diverse directed test set

Estimated time:

- 3 to 6 months

This is the range I would treat as the realistic target if you want the result
to be technically respectable instead of a narrow demo.

### 3. Near-complete VT-x coverage

Goal:

- Broad architectural coverage of VMX controls, state checks, exit reasons,
  secondary controls, APIC-related behavior, paging interactions, and a large
  fraction of SDM-defined corner cases

Estimated time:

- 6 to 12+ months

This is the range if you want something approaching "Intel VT-x support" in the
normal sense people will assume when they read the claim.

## Why It Takes That Long

VMX is not just a handful of new opcodes. It cuts across:

- x86 privilege and exception rules
- control register handling
- segment validity rules
- guest/host architectural state storage
- instruction decode and execution
- address translation and memory typing
- interrupt and event injection
- fault prioritization
- debug and performance-sensitive execution paths

The main time cost is not writing structs. It is getting the architectural
checks, transitions, and side effects correct enough that `VMLAUNCH` and
`VMRESUME` either succeed legally or fail in the right way.

## Required Work Breakdown

### 1. VMX architecture state model

You need architectural per-core VMX state, not just a VMCS class.

Required state includes:

- whether the core is in VMX root operation
- whether `VMXON` has been executed
- the physical address of the VMXON region
- the current VMCS pointer
- whether a current VMCS is active on this logical CPU
- whether the current VMCS is clear or launched
- pending VM-entry failure state
- latched controls derived from model-specific registers if you implement those
  checks faithfully

Likely code touch points:

- x86 thread or CPU architectural state classes
- serialization / checkpoint support if needed
- reset paths

### 2. VMX capability and MSR model

A serious implementation needs at least a coherent story for the VMX-related
MSRs, especially if software reads them during initialization.

Needed items include:

- `IA32_VMX_BASIC`
- `IA32_FEATURE_CONTROL`
- pinbased controls MSR
- primary processor-based controls MSR
- exit controls MSR
- entry controls MSR
- secondary processor-based controls MSR if enabled
- CR0/CR4 fixed-bits MSRs
- any capability MSRs your target software checks

This requires decisions about:

- which VMX features gem5 claims to support
- what fixed control bits are exposed
- whether unsupported controls reject `VMWRITE`, VM-entry, or both

### 3. VMCS data model

The current `vmcs.hh` is not enough. You need an architectural VMCS model.

At minimum:

- the 4 KiB VMCS region abstraction
- the region header semantics
- field encodings
- field width and access metadata
- storage for all implemented guest-state fields
- storage for all implemented host-state fields
- storage for control fields
- storage for read-only exit information fields

Strongly recommended implementation structure:

- one enum for VMCS field encodings
- one metadata table describing width, access type, and category
- one storage container keyed by field encoding
- helper accessors that enforce width and legality

Without this layer, `VMREAD`, `VMWRITE`, VM-entry checks, and VM-exit population
all become inconsistent and fragile.

### 4. VMX instruction semantics

The following instructions need full architectural behavior:

- `VMXON`
- `VMXOFF`
- `VMCLEAR`
- `VMPTRLD`
- `VMPTRST`
- `VMREAD`
- `VMWRITE`
- `VMLAUNCH`
- `VMRESUME`
- `VMCALL`

For each instruction you need:

- privilege checks
- `CR4.VMXE` checks
- protected mode / long mode preconditions as required
- memory operand decoding rules
- alignment checks
- current-VMCS rules
- carry / zero flag success and failure reporting
- `#UD`, `#GP`, `#PF`, and VMfail behavior where applicable

The hardest instructions are `VMLAUNCH` and `VMRESUME`, because they require:

- validating the current VMCS
- loading guest state
- loading controls
- deciding whether VM-entry fails before guest execution starts
- transferring execution into guest context

### 5. VM-entry checks

This is a major block of work on its own.

You need consistency checks for:

- guest control registers
- guest segment selectors, bases, limits, and access rights
- task register and LDTR validity
- guest RIP, RSP, and RFLAGS constraints
- host segment requirements
- host control register requirements
- canonical-address rules in 64-bit mode
- control field dependencies
- event injection consistency
- activity state and interruptibility state

If you skip too many of these, `VMLAUNCH` may "work" for one case but will not
be compliant in any defensible sense.

### 6. Guest/host state transfer

You need code that can:

- save host architectural state into the conceptual host context
- load guest architectural state from the VMCS
- on VM exit, write guest state back to the VMCS
- restore host state from host fields

This will touch:

- general registers
- RIP / RSP / RFLAGS
- control registers
- debug registers if implemented
- segment registers and descriptor tables
- SYSENTER / SYSCALL-related state if required by your chosen scope

### 7. VM-exit generation

You need a dispatch path that can recognize when guest execution must leave the
guest and return to VMX root operation.

Minimum categories:

- external interrupt or NMI exits if you support them
- CPUID exit
- HLT exit
- control register access exit
- I/O instruction exit
- MSR read/write exit
- EPT violation exit if EPT is implemented
- exception-based exits for any exceptions you choose to trap
- `VMCALL`

Each exit needs:

- exit reason
- exit qualification where architecturally defined
- guest linear address if applicable
- guest physical address if applicable
- instruction length where required
- interruption-information fields when applicable

### 8. Event injection and interruptibility

For VM entry and certain exits, you need:

- interrupt injection metadata
- exception injection rules
- NMI injection rules if supported
- interruptibility-state tracking
- blocking by STI / MOV SS behavior if modeled

This area is easy to underbuild and then spend weeks debugging.

### 9. Paging and memory virtualization strategy

You must decide early whether the initial target includes:

- guest linear to guest physical only, reusing gem5 paging
- or full guest physical to host physical translation with EPT

If you include EPT:

- timeline increases substantially
- MMU and TLB work expands
- access/dirty semantics become more complex
- page-walk and caching behavior need design decisions

Recommendation for first milestone:

- do not start with EPT unless your target workload requires it immediately

### 10. Exception and fault prioritization

You need to define how VMX interacts with the existing x86 fault model.

Important areas:

- when an instruction should fault before VMX processing
- when VM instruction errors should be reported through flags versus exceptions
- when guest exceptions become VM exits
- ordering between page faults, general protection faults, and VMfail cases

This requires careful integration with existing x86 execution semantics.

### 11. Decoder and instruction implementation plumbing

The decoder already recognizes some opcodes, but architectural behavior still
needs instruction implementations and microcode or macroop integration.

Tasks include:

- replacing `WarnUnimpl` entries
- binding all VMX opcodes to real implementations
- ensuring operand-size and mode restrictions are correct
- making tracing and disassembly intelligible

### 12. Debugging and observability

VMX is difficult to debug without dedicated tracing.

Strongly recommended:

- VMX transition trace messages
- VMCS dump helpers
- VM-entry failure trace output
- exit reason and qualification logging
- assertion-backed invariants in debug builds

Without this, schedule risk increases sharply.

### 13. Testing strategy

You will need far more than unit tests.

Recommended test layers:

- unit tests for VMCS field access and encoding
- unit tests for VM instruction precondition checks
- directed architectural tests for each VMX instruction
- VM-entry success and failure test cases
- VM-exit reason tests
- guest/host state transfer tests
- negative tests for illegal states

Useful early milestones:

- a tiny hand-written VMX bring-up program in assembly
- a "known-bad VMCS" suite to validate VM-entry failure reasons
- a small guest that exits on `CPUID`, `HLT`, and `VMCALL`

### 14. Documentation

The implementation needs documentation in parallel with code, not at the end.

Needed docs:

- supported VMX feature matrix
- unsupported features and deliberate simplifications
- VMCS field coverage table
- testing strategy
- known compliance gaps versus Intel SDM

## Suggested Milestones

### Milestone 0: Design and capability contract

Estimated time:

- 1 to 2 weeks

Deliverables:

- feature subset definition
- VMX MSR policy
- VMCS field list for first implementation
- test plan

### Milestone 1: VMX root operation and VMCS management

Estimated time:

- 2 to 3 weeks

Deliverables:

- `VMXON`, `VMXOFF`, `VMCLEAR`, `VMPTRLD`, `VMPTRST`
- VMX architectural state
- minimal VMCS field infrastructure
- basic tests

### Milestone 2: VMREAD / VMWRITE and VMCS legality

Estimated time:

- 1 to 2 weeks

Deliverables:

- field encoding access layer
- width and access checks
- initial VM instruction error reporting

### Milestone 3: First VM entry and exit path

Estimated time:

- 2 to 4 weeks

Deliverables:

- `VMLAUNCH`, `VMRESUME`
- guest/host state transfer for a narrow supported mode
- exits for `CPUID`, `HLT`, `VMCALL`
- directed tests

### Milestone 4: Broader compliance work

Estimated time:

- 4 to 12 weeks

Deliverables:

- more control fields
- more consistency checks
- more exit classes
- better fault ordering
- better docs and diagnostics

## Major Risks

### 1. Scope collapse

The fastest way to lose months is to say "VT-x support" without freezing a
feature subset.

### 2. VM-entry correctness

Most schedule pain will cluster around VM-entry validation and host/guest state
switching, not around opcode decoding.

### 3. EPT pressure

If the project quietly assumes EPT from day one, timeline risk rises sharply.

### 4. Test weakness

If you do not build directed VMX tests early, you can spend a long time chasing
bugs that all present as "VMLAUNCH failed."

### 5. SDM interpretation gaps

Intel VMX behavior is spread across many sections and is easy to misread when
implementing error paths and special cases.

## Recommended First-Cut Feature Set

If the goal is to get something real working in reasonable time, I would target
this first:

- x86-64 guest only
- VMX root and non-root transitions
- `VMXON`, `VMXOFF`, `VMCLEAR`, `VMPTRLD`, `VMPTRST`
- `VMREAD`, `VMWRITE`
- `VMLAUNCH`, `VMRESUME`
- guest execution with exits on `CPUID`, `HLT`, `VMCALL`
- no EPT in v1
- no nested virtualization
- no APIC virtualization features
- no posted interrupts
- no shadow VMCS

That scope is the best chance of landing something meaningful in the 6 to 10
week range.

## Bottom-Line Estimate

If you want a serious answer for this specific tree:

- small controlled VMX prototype: about 6 to 10 weeks
- solid research-grade VMX support: about 3 to 6 months
- broad Intel-like VT-x coverage: about 6 to 12+ months

If the work is part-time, multiply accordingly. If this is your first deep VMX
implementation, expect the upper half of each range.
