# gem5 Intel VMX Support

This branch extends gem5's x86 architecture model with a research-oriented
implementation of Intel Virtual Machine Extensions (VMX). The current model
supports the foundational VMX lifecycle required to enter VMX operation,
construct and manage VMCS state, launch a 64-bit guest, transition between VMX
root and non-root operation, handle supported VM exits, resume guest execution,
and return to normal host execution.

The implementation follows the architectural behavior described in the
[Intel 64 and IA-32 Software Developer's Manual, Volume
3C](https://www.intel.com/content/www/us/en/content-details/868148/intel-64-and-ia-32-architectures-software-developer-s-manual-volume-3c-system-programming-guide-part-3.html).

This is intentionally not an implementation of every facility provided by
modern Intel VT-x hardware. Instead, the project focuses on building an
architecturally accurate, inspectable, and testable virtualization model
suitable for gem5-based systems and architecture research.

In addition to VMX itself, this branch contains substantial changes to gem5's
x86 memory-management and page-walking infrastructure. These changes repair
architectural correctness problems in the existing paging path and establish
the translation machinery required for future Extended Page Table (EPT)
support.

## Project status

The implemented VMX model currently provides:

* VMX root and non-root execution state
* VMXON and VMCS region management
* VMCS field storage and lifecycle state
* VM-entry validation and guest-state loading
* VM-exit guest-state saving and host-state restoration
* VM-instruction success and failure semantics
* selected VM-exit interception behavior
* VM-entry event injection for supported event classes
* VMX checkpoint serialization
* corrected x86 page-walking behavior and MMU coordination
* regression testing across multiple CPU and memory-system configurations

Extended Page Tables are under active development. The current branch contains
the prerequisite paging and MMU infrastructure needed to support nested
guest-virtual to guest-physical to host-physical translation, but EPT is not
yet advertised as an architectural capability.

## Implemented VMX instructions

The following instructions have functional behavior in VMX root and non-root
operation:

* `VMXON` and `VMXOFF`
* `VMCLEAR`, `VMPTRLD`, and `VMPTRST`
* `VMREAD` and `VMWRITE`
* `VMLAUNCH` and `VMRESUME`
* `VMCALL`

The implementation checks architectural conditions including:

* instruction recognition and encoding
* privilege level
* VMX root and non-root state
* `IA32_FEATURE_CONTROL`
* `CR4.VMXE`
* VMX CR0 and CR4 fixed-bit requirements
* physical-address validity
* VMX-region alignment
* VMCS revision identifiers
* current and launched VMCS state

VMX instructions report success, VMfailInvalid, VMfailValid, `#UD`, and `#GP`
using Intel-defined architectural behavior and RFLAGS state.

VMX instructions executed while in VMX non-root operation generate the
appropriate VM exit rather than being rejected by an early decode-time
privilege check.

## VMCS and VMX state

VMCS regions retain their architectural physical addresses while being
associated with typed simulator-side VMCS state.

The model tracks:

* the current VMCS pointer
* clear and launched state
* active VMCS state
* VM-instruction errors
* VM-entry control state
* VM-exit information
* guest architectural state
* host architectural state
* guest/host masks and read shadows
* event-injection information

VMX execution state is included in gem5 checkpoint serialization and survives
supported CPU model transitions.

VMREAD and VMWRITE support the implemented VMCS field allowlist, including
natural-width, 32-bit, and 64-bit fields and Intel's high-half access encoding
where applicable.

## VMX capability model

VMX capability MSRs are intentionally conservative.

An advertised allowed-one control bit is treated as a promise that the
corresponding behavior is implemented and sufficiently validated. Features
that are only partially wired internally are therefore not automatically
exposed to guest software.

The current capability model includes:

* a 4 KiB VMCS region
* VMCS revision identification
* write-back VMCS memory type
* true VMX controls
* CR0 and CR4 fixed-bit requirements
* 64-bit host execution
* 64-bit IA-32e guest execution
* guest and host `IA32_EFER` transitions
* CR3-load exiting as the supported optional primary processor control

All pin-based execution controls are currently required to be zero.

The model does not advertise controls for facilities whose complete
architectural contract is not yet implemented, including interrupt
virtualization, I/O bitmaps, MSR bitmaps, VPID, and EPT.

The complete feature and verification matrix is documented in
[the VMX architectural audit](docs/vmx-architectural-audit.md).

## VM entry

`VMLAUNCH` and `VMRESUME` validate VMCS control and architectural state before
entering VMX non-root operation.

Validation includes:

* VMX control capability masks
* CR0 and CR4 fixed-bit requirements
* CR3 validity
* paging and IA-32e relationships
* `IA32_EFER`
* canonical addresses
* segment selectors
* segment bases and limits
* access-right fields
* descriptor tables
* task-state information
* guest activity state
* guest interruptibility state
* VMCS link pointer state
* supported event-injection state

Successful VM entry loads guest architectural state from the VMCS, including:

* CR0, CR3, and CR4
* segment state
* descriptor-table state
* task and local-descriptor state
* debug state
* `RSP`
* `RIP`
* `RFLAGS`
* `IA32_EFER`
* supported SYSENTER state

The guest then begins execution in VMX non-root operation.

## VM exits

On a supported VM exit, gem5 records architectural exit information such as:

* exit reason
* exit qualification
* instruction length
* guest linear address where applicable

Guest architectural state is saved into the VMCS before host state is loaded.

Host restoration uses the architectural host-state area of the VMCS rather
than relying on a simulator-only snapshot captured before VM entry.

Supported or partially supported exit paths include:

* `VMCALL`
* CR0 and CR4 accesses governed by guest/host masks
* configured CR3-load exits
* exception-bitmap exits
* `CPUID`
* VMX instructions executed in non-root operation
* unconditional non-root `RDMSR` and `WRMSR` exit paths

MSR bitmap filtering and complete end-to-end validation of all MSR-related
exit cases are not yet supported.

Other interception hooks may exist internally, but controls are not advertised
until their validation, execution semantics, exit information, and regression
coverage are complete.

## VM-entry event injection

The VM-entry path supports architectural injection of selected event classes,
including:

* external interrupts
* NMIs
* hardware exceptions
* software interrupts
* privileged software exceptions
* software exceptions

The implementation validates event type, vector, error-code requirements,
instruction length, and relevant guest interruptibility-state rules before
guest execution begins.

Event injection currently targets the supported 64-bit guest environment.
Some advanced interruptibility and legacy-mode behaviors remain outside the
current project scope.

## x86 paging and MMU rework

Implementing EPT safely requires a correct first-stage x86 translation model.
During VMX development, a number of architectural and simulator-integration
issues were identified in gem5's existing x86 page-walking implementation.

The paging path has therefore been substantially reworked.

Architectural fixes include:

* correct propagation of execute-disable (`NX`) permissions
* accumulation of user/supervisor and read/write permissions across all levels
* correct accessed and dirty-bit behavior
* coherent page-table-entry updates
* reserved-bit validation
* physical-address-width validation
* large-page alignment validation
* correct PAT-bit extraction
* corrected legacy 4 MiB page handling
* legacy PAE PDPTE retention and validation
* canonical-address checking
* improved page-fault address handling
* CR0, CR3, and CR4 validation
* PCID context separation

The simulator-side MMU design has also been restructured.

Translation coordination now resides in the x86 MMU rather than the TLB. The
TLB acts primarily as a translation cache, while page-table walks operate from
an immutable translation-context snapshot.

The new translation path includes:

* immutable `TranslationContext` capture
* centralized MMU coordination
* queued `PagingPort` transport
* typed page-walker results
* translation-generation tracking
* stale-walk detection
* explicit walk lifecycle and drain accounting
* improved O3 squash behavior
* correct handling of paging-control-register changes
* split-access validation
* improved instruction and data translation concurrency

These changes are intended both to improve baseline x86 architectural
correctness and to provide the infrastructure required for nested EPT walks.

The base paging changes were regression-tested across 56 CPU and memory-system
configurations, including AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU,
Classic cache hierarchies, and Ruby MESI Two Level configurations. Tests were
used to verify unchanged tick, instruction, and TLB behavior in cases where the
architectural behavior should remain equivalent to the baseline.

## Extended Page Table development

EPT is the next major architectural component of the project.

The intended translation path is:

```text
Guest Virtual Address
        |
        v
Guest page-table walk
        |
        v
Guest Physical Address
        |
        v
Extended Page Table walk
        |
        v
Host Physical Address
```

The paging and MMU restructuring described above establishes the foundation
for this two-dimensional translation process.

Ongoing EPT work includes:

* EPT pointer and VMCS control support
* secondary processor-based VM-execution controls
* nested page-walk coordination
* concurrent guest and EPT walk state
* EPT read/write/execute permissions
* EPT violations
* EPT misconfigurations
* guest-physical-address reporting
* VM-exit qualification
* accessed and dirty-bit behavior where supported
* memory-type and PAT/PWT/PCD resolution
* TLB interaction and invalidation behavior
* end-to-end nested-translation tests

EPT will not be advertised through VMX capability MSRs until the complete
architectural contract supported by this project is implemented and tested.

## Related x86 instruction support

A VMM must collect host architectural state before populating a VMCS. gem5's
baseline x86 implementation provides several instructions required for this
without simulator-specific shortcuts:

* `SGDT` stores the global-descriptor-table limit and base.
* `SIDT` stores the interrupt-descriptor-table limit and base.
* `SLDT` returns the local-descriptor-table selector.
* `STR` returns the current task-register selector.

These instructions use gem5's standard architectural implementations.

`SGDT` and `SIDT` store the packed x86 pseudo-descriptor, including the legacy
16-bit operand-size layout where required. `SLDT` and `STR` use the standard
register-or-memory instruction paths to return their 16-bit selectors.

This allows VMM code running inside the simulated machine to construct VMCS
host state using ordinary x86 instructions rather than gem5-specific access
mechanisms.

## Debugging

The `VMX` debug flag prints traces of important VMX state transitions,
including:

* VMXON
* VMCS selection
* VM-entry validation
* guest-state loading
* VM exits
* guest-state saving
* host-state restoration
* VMRESUME
* VMXOFF

Example:

```sh
build/X86/gem5.opt --debug-flags=VMX --debug-file=vmx.trace \
    <full-system-config.py> <config arguments>
```

## Building

Build the x86 optimized binary using the normal gem5 build process:

```sh
scons build/X86/gem5.opt -j"$(nproc)"
```

VMX testing requires full-system mode because VMX instructions are privileged.

An out-of-tree loadable kernel module can be used to:

1. enable VMX through CR4,
2. allocate VMXON and VMCS regions,
3. enter VMX operation,
4. configure a minimal VMCS,
5. launch a guest payload,
6. validate expected VM exits,
7. resume the guest,
8. leave VMX operation,
9. restore host state and free allocated resources.

Test modules, generated objects, kernel modifications, and test-only disk
images should remain outside the gem5 source repository.

## Validated VMX lifecycle

The basic development lifecycle is:

```text
VMXON
  -> VMCLEAR
  -> VMPTRLD
  -> VMWRITE / VMREAD
  -> VMLAUNCH
  -> guest execution
  -> VMCALL VM exit
  -> host execution
  -> VMRESUME
  -> guest execution
  -> VMCALL VM exit
  -> VMXOFF
```

This lifecycle has been validated using an external Linux kernel module in a
gem5 full-system environment.

Additional tests cover VMCS metadata, instruction result flags, launch and
resume state, VM-entry validation, CR virtualization, selected exception
exits, paging behavior, CPU-model interaction, and regression behavior outside
VMX execution.

## Current limitations

The following facilities are not currently part of the supported VMX contract:

* Extended Page Tables (EPT), currently under active development
* Virtual Processor Identifiers (VPID)
* unrestricted guests
* real-mode guests
* external-interrupt exiting
* NMI exiting
* interrupt-window and NMI-window exiting
* HLT exiting
* INVLPG exiting
* RDTSC exiting
* CR8 access exiting
* unconditional-I/O exiting
* I/O bitmaps
* MSR bitmaps
* complete VM-entry and VM-exit MSR load/store lists
* APIC virtualization
* virtual interrupts
* posted interrupts
* VM functions
* VMX preemption timer
* SMM dual-monitor treatment
* nested VMX
* complete multi-vCPU VMCS ownership semantics
* running an unmodified general-purpose hypervisor as a nested guest

Some internal instruction or interception paths may exist for features listed
above. They are intentionally not advertised through VMX capability MSRs until
their full architectural behavior is implemented and validated.

## Research goal

The goal of this project is not to reproduce every optional Intel VT-x
facility.

The goal is to provide gem5 with a sufficiently accurate and testable model of
the foundational Intel virtualization architecture to support research into:

* VM-entry and VM-exit behavior
* VMCS operation
* virtualization instruction timing
* address-translation overhead
* nested page walking
* Extended Page Tables
* MMU and TLB behavior under virtualization
* architectural and microarchitectural virtualization experiments

The implementation follows a conservative capability model: features are
advertised only when their state validation, execution behavior, architectural
side effects, exit behavior, and simulator lifecycle interactions are
implemented together.
