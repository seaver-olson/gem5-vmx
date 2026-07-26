# VMX architectural audit

## Scope, status, and conclusion

This audit was refreshed against `c9a1a2efcc` on 2026-07-26.  The only commit
after the original VMX hardening change (`8f48cd2035`) deletes root-level
workspace notes; it does not change VMX source, tests, or this audit.  The
source review below therefore applies to the current branch, including the
narrow repair recorded in the findings table.

The primary architectural reference is the Intel 64 and IA-32 Architectures
Software Developer's Manual, Volume 3C, June 2026, order number
326019-092US.  Intel's current manual index still lists version 092.  Section
numbers below refer to that edition.  The simulator-integration reference is
this branch's x86 ISA, decoder, fault, misc-register, MMU/TLB, CPU-switch, and
serialization machinery.

The intended contract is intentionally narrow: one logical processor running a
controlled 64-bit host and 64-bit IA-32e guest without
EPT, VPID, APIC virtualization, event injection, or MSR lists.  Within that
target, VM entry changes the page-table root used by instruction and data
translation, guest CR3 writes use the normal gem5 x86 CR3 path, and VM exit
restores the host translation and execution state before the handler runs.
Capabilities have been reduced to that implementation.  This is suitable for
foundational single-vCPU VMX experiments after the recorded regressions are
re-run; it is not a general virtualization platform or a model for
interrupt-heavy, nested-OS, or multiprocessor VMX research.

The prior integration test run remains useful evidence, but it is not evidence
that the current checkout has been revalidated: the full-system resources
needed to repeat it are not present in this workspace.  The **Recorded
verification** section preserves the exact historical commands and results
separately from this source audit.

A repository-wide file and marker search was followed by detailed review of
the VMX-modified paths and their callers.  The reviewed subsystems include:

- VMX decoding, mandatory prefixes, ModRM forms, effective addresses, and
  register/memory operand handling;
- every implemented VMX instruction and its status/fault paths;
- `VmxState`, VMCS metadata/storage/lifecycle, VMXON regions, and checkpoint
  state;
- VM entry validation, state loading, commit ordering, and late failures;
- VM exit metadata, guest saving, host loading, and execution redirection;
- x86 CR0/CR3/CR4/CR8 micro-ops, CLTS/LMSW, derived mode state, MMU/TLB
  behavior, and page-table walker use of CR3;
- segments, descriptor caches, EFER, RIP/RSP/RFLAGS, exceptions, interrupts,
  I/O, MSRs, and instruction intercept hooks;
- CPUID, VMX capability/fixed-bit MSRs, KVM-to-Atomic CPU switching, SCons,
  tests, scripts, documentation, and relevant history; and
- TODO/FIXME/XXX/HACK/workaround/stub/unsupported/panic/fatal/warn and direct
  architectural-state manipulation searches across the repository.

## How to read this audit

"Implemented" means the code provides the stated behavior in the single-vCPU
contract.  "Partially implemented" identifies an intentional boundary, an
open validation gap, or incomplete runtime coverage.  "Recognized and
rejected" means entry requires zero, an enabling capability is absent, or
access fails architecturally.  A typed enum or dormant helper is not support.

## Supported feature matrix

| Feature | Audited status | Contract and regression evidence |
|---|---|---|
| VMXON | Partially implemented | CPL, mode, CR4.VMXE, fixed bits, locked `IA32_FEATURE_CONTROL`, operand faults, 4-KiB alignment, 48-bit physical width, VMXON-pointer uniqueness, and revision are checked. Physical-access machine checks and cross-CPU ownership are outside the model. Smoke test covers success. |
| VMXOFF | Partially implemented | Root/non-root conditions, status flags, current/active VMCS cleanup, and VMX state reset are modeled. Only one logical processor is supported. Smoke test covers success and cleanup. |
| VMCLEAR | Implemented for one CPU | Does not inspect the revision field (SDM 33 VMCLEAR), preserves implementation-specific VMCS fields, clears launch/active state, and removes the current pointer when applicable. Smoke and transition tests cover revision independence and lifecycle. |
| VMPTRLD | Partially implemented | Checks address, VMXON alias, revision, and shadow indicator; selects the per-CPU current VMCS and marks it active. Active ownership by another logical processor is not represented. Smoke test covers success and error 11. |
| VMPTRST | Implemented for one CPU | Stores the current pointer or all ones through the normal memory/fault path. Smoke and restore tests cover live state. |
| VMREAD / VMWRITE | Implemented for the supported field allowlist | Width truncation/extension, natural width, full/high 64-bit access, reserved encodings, read-only fields, memory operands, and VMfailInvalid/Valid are modeled. Unit tests cover metadata and high-half behavior; smoke covers architectural round trip. |
| VMLAUNCH / VMRESUME | Partially implemented | Correct launch-state requirements, pre-commit VMfail, supported control/host/guest validation, late guest-state failure, guest state load, and launch transition are modeled. Transition test covers success, resume, errors 5/7, late failure, and atomic early failure. VM entry immediately after `MOV SS` is outside the verified contract because gem5 does not expose the blocking shadow. |
| VMCALL | Implemented for the narrow contract | Non-root execution exits with reason 18 and length 3; root execution reports the VM-instruction result. Transition test covers exit, saved RIP, and resume. |
| VMX root / non-root operation | Partially implemented | Per-ISA state is integrated with ordinary instruction/fault paths and CPU switching. SMM dual-monitor, nested VMX, and multi-vCPU ownership are excluded. |
| VMCS lifecycle | Partially implemented | Current, active, clear, and launched state are explicit and serialized. Ordinary/shadow headers are distinguished. Cross-logical-processor active ownership is not modeled. |
| VM-entry validation | Implemented; integration verification pending | Capability masks, counts, host/guest control state, selectors, bases, limits, access rights, RFLAGS, EFER, canonicality, activity, interruptibility zero, link pointer, CR3, paging relationships, and guest `IA32_SYSENTER_ESP/EIP` are checked. A focused transition case covers the new failure path, but cannot run without the full-system resources. Unsupported counts/event injection fail with error 7. |
| VM-entry state loading | Implemented for verified 64-bit mode | Guest CR0/3/4, EFER, segments/caches, tables, SYSENTER, DR7 reset state, RIP/RSP/RFLAGS, decoder mode, and MMU context are installed through gem5 interfaces. Transition test proves execution at guest RIP/RSP and guest translations. |
| VM exits | Partially implemented | The common path records supported metadata, saves guest state before replacement, loads coherent host state, leaves non-root mode, and redirects to host RIP. Advanced exit causes and abort handling remain limited. |
| Guest-state save / host-state load | Implemented for accepted state | Includes CR0/3/4, DR7, segments, tables, SYSENTER, EFER when selected, RIP/RSP/RFLAGS, and mode/MMU updates. Transition test proves guest save and immediate host mapping restoration. |
| VM-instruction errors and CF/ZF | Implemented for the instruction subset | Success clears CF/ZF, VMfailInvalid sets CF, VMfailValid sets ZF and updates error state without changing other RFLAGS. Unit/smoke/transition tests cover representative outcomes and errors 5, 7, and 11. |
| Exit reason, qualification, and length | Partially implemented | VMCALL, CR access, and directly intercepted #PF are integration-tested. Shared structures support other existing hooks, but every Intel exit form is not claimed. |
| CR0/CR4 virtualization | Implemented foundational subset | Guest/host masks and read shadows are writable and consulted by MOV-CR, CLTS, and LMSW. Non-exiting writes merge owned bits and use normal CR writes. Transition test covers CR0 shadow reads and CLTS qualification. |
| CR3 virtualization | Implemented foundational subset | CR3-load exiting is the only optional primary control. CR3 targets are unavailable (count must be zero). Mode/reserved/physical checks and normal gem5 CR3/MMU side effects are used. Transition test proves non-exiting switch and fault-like exiting switch. |
| CR8 virtualization | Recognized, not advertised | Existing hooks do not constitute support; CR8 load/store exit controls cannot be enabled. |
| Exception bitmap | Partially implemented | Writable bitmap and #PF mask/match are consumed by the common fault path. Direct #PF metadata and CR2 preservation are tested. Async/abort-class coverage is limited as described below. |
| CPUID and VMX-instruction exits | Implemented | CPUID and all VMX instructions exit unconditionally in non-root operation as Intel defines. VMCALL is integration-tested; the other paths are code-reviewed but not all have byte-level runtime tests. |
| INVD / GETSEC / XSETBV | Partial interception only | Non-root execution takes the unconditional VM exit with its distinct reason. Root execution retains baseline gem5's warning/no-op implementation and is outside this VMX research contract. |
| HLT, INVLPG, MOV-DR, RDTSC exits | Recognized, not advertised | Their optional primary control bits are required zero. RDTSCP is unavailable because its secondary enable control is unavailable and therefore raises `#UD` in non-root operation. |
| RDMSR / WRMSR | Partially implemented | With MSR bitmaps unavailable, non-root RDMSR/WRMSR use the architectural unconditional-exit path. No end-to-end MSR-exit regression is included. |
| I/O exits and bitmaps | Not advertised | Unconditional-I/O and I/O-bitmap controls are unavailable, avoiding incomplete string/REP exit metadata. Ordinary non-exiting I/O remains baseline gem5 behavior. |
| External interrupts / NMIs | Not advertised as VM exits | All pin controls are required zero. Guest interruptibility/shadow semantics are not complete enough for an interrupt-heavy contract. |
| Interrupt/NMI-window exits | Not implemented, not advertised | The enabling primary controls are unavailable. |
| MSR load/store lists | Recognized and rejected | All three counts must be zero during entry; list execution is absent. |
| VM-entry event injection | Recognized and rejected | The entry interruption-information field must be zero. |
| EPT / VPID | Not implemented, not advertised | Secondary-control and EPT/VPID capability MSRs are unavailable. TLB state is therefore transitioned as VPID 0. |
| Unrestricted guest | Not implemented, not advertised | CR0 fixed bits and required IA-32e controls exclude real-mode/unpaged guests. |
| APIC virtualization / posted interrupts | Not implemented, not advertised | Related pin/secondary controls and components are unavailable. |
| VMCS shadowing | Not implemented, not advertised | A shadow indicator is rejected; the simulator's internal typed VMCS is not architectural shadowing. |
| Nested VMX | Not implemented, not advertised | VMX instructions in non-root operation exit to the modeled VMM. |

## Capability contract

Every allowed-one bit is treated as an implementation promise.  The smoke
test recorded below reads this exact contract after a KVM-to-Atomic CPU switch,
which also prevents physical-host capabilities from leaking into the simulated
CPU.

| Architectural interface | Reported value / rule |
|---|---|
| CPUID.01H:ECX.VMX | 1 |
| `IA32_FEATURE_CONTROL` | locked; VMXON outside SMX enabled; writes fault |
| `IA32_VMX_BASIC` | revision 1; 4096-byte region; WB memory type; true controls available |
| Pin-based controls | required-one 0; allowed-one 0 |
| Primary processor controls | required-one 0; allowed-one bit 15 only (CR3-load exiting) |
| VM-exit controls | host-address-space-size bit 9 required; bits 9, 20 (save EFER), and 21 (load EFER) allowed |
| VM-entry controls | IA-32e-guest bit 9 required; bits 9 and 15 (load EFER) allowed |
| True controls | identical to the four legacy control MSRs |
| `IA32_VMX_MISC` | 0, including zero CR3-target capacity |
| `IA32_VMX_CR0_FIXED0/FIXED1` | protected, paged foundational VMX contract; CR0 loaded bits only |
| `IA32_VMX_CR4_FIXED0/FIXED1` | VMXE required; PCIDE not allowed |
| `IA32_VMX_VMCS_ENUM` | 0x2a, matching the highest supported component index |
| Secondary/tertiary controls, EPT/VPID, VMFUNC, exit-controls2 | MSRs are unmapped and `RDMSR` faults instead of returning permissive zero/all-one masks |

## Findings and repairs

The state column records repair and verification status, as well as behavior
excluded by the capability contract.

| ID | Classification / severity | Intel rule and gem5 mechanism | Defect found | Repair and proving test | State |
|---|---|---|---|---|---|
| A-01 | Critical architectural error | SDM 29.3.2, 30.3.1, 30.5.1; gem5 effective segment bases | RIP save/load used raw CS base, which is ignored for 64-bit code. | Use `CsEffBase`; transition test reaches exact guest RIP and verifies saved VMCALL RIP. | **Closed** |
| A-02 | Critical architectural error | SDM 29.2.2, 29.3.1.1, 30.5.1; `ISA::setMiscReg(Cr3)` and MMU walker | CR3 checks ignored mode-dependent low bits and PCID consistency; no test proved the walker changed roots. | Add mode-aware CR3 validation, exclude PCIDE, write CR3 through the architectural interface, and flush the VPID-0 transition context. Unit tests cover encodings; transition maps one VA to three physical pages and proves all three. | **Closed** |
| A-03 | Major correctness error | SDM 29.3.2.1 and 30.5.1 | Entry/exit overwrote CR0 bits that VMX does not load. | Merge only the Intel-loaded CR0 bits with live CR0 before the normal gem5 write. Unit tests protect the mask; transition exercises mask/shadow behavior. | **Closed** |
| A-04 | Missing validation / narrow correctness gap | SDM 29.3.1.1 | Guest `IA32_SYSENTER_ESP/EIP` were loaded without the canonical-address checks required on Intel 64 processors. The SDM does not impose the `IA32_SYSENTER_CS` validation previously suspected, and host ESP/EIP checks were already present. DR7 reset behavior is implemented. | Validate both guest addresses before state loading and make `GUEST_SYSENTER_EIP=1<<48` take the existing late VM-entry-invalid-guest-state path. The focused transition case checks preserved exit information, clear launch state, and restored host mapping. | **Fixed in code; integration pending** |
| A-05 | Missing validation | SDM 25.6, 29.2.1, 29.3.1 | Consumed foundational fields were absent from VMCS metadata, causing architectural VMWRITE failures followed by internal default-zero reads. | Add all consumed foundational fields, widths, high access, and read-only metadata; require unsupported counts/injection to be zero; correct VMCS enumeration. Nine VMCS unit cases plus integration entry cover this. | **Closed** |
| A-06 | Missing interception / major correctness | SDM 30.2.1, 30.4 and Table 30-1; gem5 common fault path | Exception fields were unreachable; direct #PF treatment and software-exception lengths/debug qualification were incomplete. | Route supported synchronous exceptions through the shared VMX fault path; direct #PF writes the faulting linear address to qualification and does not update CR2; add software length and #DB qualification. Transition proves #PF metadata/CR2. | **Closed for synchronous tested subset** |
| A-07 | Incomplete feature / capability mismatch | SDM Appendix A and 28.1 | Broad pin, I/O, bitmap, HLT, INVLPG, MOV-DR, and CR8 controls were advertised without complete end-to-end behavior or exit metadata. | Reduce allowed controls to CR3-load exiting, required 64-bit modes, and optional EFER save/load; unmap optional capability MSRs. Smoke asserts exact masks after CPU switching. | **Restricted safely** |
| A-08 | Major correctness error | SDM 33 VMX instruction reference; gem5 decoder and memory request flags | Prefix matching was permissive; address-size override could be mistaken for RIP-relative addressing; VMX memory operands dropped segment flags and checked only their first byte for canonicality. | Require exact mandatory prefixes, use decoded address size, propagate segment/address flags, and check the full operand range for `#SS`/`#GP`. Build-generated decoder and instruction smoke verify accepted encodings; explicit byte/fault differential coverage remains a test limitation. | **Closed in code; differential test pending** |
| A-09 | Major correctness error | SDM 27, 33 VMCLEAR/VMPTRLD/VMXOFF | VMCLEAR incorrectly inspected revision data and destroyed typed fields; launch/active transitions and VMXOFF cleanup were incomplete. | VMCLEAR preserves fields and clears launch/active state without reading the revision; VMPTRLD performs the revision check; VMXOFF clears active state. Unit, smoke, and transition lifecycle tests cover this. | **Closed for one CPU** |
| A-10 | Simulator-integration error | SDM 26.8 and 28.1.3; shared gem5 CR micro-ops | Root MOV-to-CR could violate VMX fixed bits; non-root CR0/4 masks were not merged consistently; CLTS/LMSW were conflated. | Enforce root fixed bits in the shared CR write path; add CR read-shadow/write-mask merging and separate CLTS/LMSW exit rules. Seven utility unit cases and transition CLTS coverage protect this. | **Closed** |
| A-11 | Simulator-integration error | gem5 CPU takeover and serialization | VMX state was omitted during CPU-model switching, while KVM host capability MSRs could overwrite the destination CPU's advertised model. Restored VMCS state accepted inconsistent current/active combinations. | Copy dynamic `VmxState`, preserve destination model capability MSRs, and validate serialized lifecycle/field state. Smoke after KVM switch and active-root checkpoint/restore cover both paths. | **Closed for root-mode checkpoint** |
| A-12 | Major correctness error | SDM 29.8 and 33 VMLAUNCH/VMRESUME | Late VM-entry failure cleared or synthesized exit-information fields beyond exit reason/qualification and marked a VMLAUNCH VMCS launched before guest-state/MSR loading had succeeded. | Separate pre-commit and post-commit failures; late failure loads host state, leaves the VMCS clear, and changes only reason/qualification. Transition preserves a prior #PF event record and immediately verifies VMRESUME error 5. | **Closed** |
| A-13 | Test weakness | Foundational VMX sections | Earlier tests proved only root lifecycle and a field round trip; kernel modules also depended on mismatched `CONFIG_MODVERSIONS` CRCs and raw CR4 writes. | Add unit, capability, instruction-status, real-translation, entry/exit, negative, and checkpoint layers. Patch every module symbol CRC from the supplied kernel and use Linux CR4-shadow helpers. | **Closed for automated layers; real-hardware differential pending** |
| A-14 | Documentation issue | Honest research contract | Documentation described dormant helpers as support and did not state the single-vCPU/64-bit boundary. | Replace it with this matrix, exact capability table, verification record, and explicit research-suitability assessment. | **Closed** |
| A-15 | Incomplete feature | SDM 26.5/26.10 | Active VMCS ownership cannot be checked across logical processors. | Configuration and documentation restrict the verified contract to one logical processor. No capability implies multiprocessor correctness; a future ownership registry is required before widening the contract. | **Restricted** |
| A-16 | Incomplete feature | SDM 33 VMLAUNCH/VMRESUME; gem5 interrupt-shadow state | VM entry must fail if the logical processor is blocking events because of `MOV SS`, but baseline gem5 has no reusable architectural shadow that the VMX instruction can query. | Do not claim this instruction sequence: document it as outside the verified contract. A shared x86 interrupt-shadow model is required before this precondition can be implemented and differentially tested. | **Restricted** |
| A-17 | Simulator-integration limitation | SDM 30.1/Table 30-1; baseline gem5 instruction semantics | INVD, GETSEC, and XSETBV were warning/no-op instructions, which also meant non-root execution failed to exit. | Add unconditional non-root VM exits with the proper distinct reasons. Retain and explicitly exclude the existing root warning/no-op semantics; implementing those three facilities is a separate baseline-x86 project. | **Closed for non-root; root restricted** |

## CR3/MMU correctness assessment

The active page-table root is never a VMX-only shadow.  Entry validates guest
CR3 for the accepted IA-32e paging mode, loads EFER/CR4/CR0 in dependency
order, then calls `ThreadContext::setMiscReg(misc_reg::Cr3, guestCr3)`.  The
standard x86 path invalidates non-global translations and the page-table
walker reads that architectural register.  The transition additionally
flushes the full modeled TLB at host/guest boundaries because VPID is absent.
The same process is used for host CR3 before host RIP/RSP are made active.

The integration regression gives the host, guest root A, and guest root B
different physical pages at the same virtual address.  It proves host data
before entry, guest-A data after entry, guest-B data after a non-exiting
MOV-to-CR3, an unchanged guest CR3 plus reason/qualification on a configured
CR3-load exit, and host data immediately at the exit handler.  This is strong
evidence for the verified 64-bit, no-PCID, no-VPID case.  Legacy PAE/PDPTE,
PCID, VPID, and EPT translation are explicitly excluded.

## VM-entry atomicity assessment

Control and host-state failures occur before state loading and return
VMfailValid without leaving root operation, changing CR3, or launching the
VMCS.  Guest-state failure is modeled after the entry commit boundary: host
state is loaded, exit reason has the entry-failure bit, qualification is zero,
the VMCS remains clear, and unrelated VM-exit information remains unchanged.
The transition regression tests both paths, immediately verifies that
VMRESUME fails with error 5, and verifies the host mapping at their handlers.
MSR-list and event-injection late-failure cases do not exist because their
controls/counts are rejected before entry.

## VM-exit host-restoration assessment

The common path saves guest state before loading host state.  Host EFER,
CR4/CR0/CR3, complete segment caches, descriptor tables, SYSENTER state, RSP,
and RIP are loaded through gem5 architectural interfaces; the decoder mode
and MMU therefore agree before handler execution.  VMCALL, CR0/CR3 exits, and
direct #PF all prove the host CR3 and host mapping at the first C handler code.
This assessment does not extend to VMX-abort/machine-check/shutdown paths or
multi-vCPU ownership, which are not modeled.

## Recorded verification

The following commands and results were recorded with the original VMX
hardening change (`8f48cd2035`, 2026-07-22).  The full recorded suite has not
been re-run during this refresh because this checkout lacks the required
full-system resources.  The VMX object and standalone unit tests were built
and run after A-04 was repaired, but they do not replace the full suite.  The
recorded suite should be re-run after any behavior change:

```sh
CC=gcc-14 CXX=g++-14 scons build/X86/gem5.opt \
    build/X86/arch/x86/vmcs.test.opt \
    build/X86/arch/x86/vmx_utils.test.opt -j4
build/X86/arch/x86/vmcs.test.opt
build/X86/arch/x86/vmx_utils.test.opt
tests/gem5/vmx/run.sh transition
tests/gem5/vmx/run.sh normal
tests/gem5/vmx/run.sh checkpoint
tests/gem5/vmx/run.sh restore
CC=gcc-14 CXX=g++-14 scons build/X86/gem5.fast -j4
git diff --check
sh -n tests/gem5/vmx/run.sh
python3 -m py_compile tests/gem5/vmx/config.py
python3 util/style.py --modifications
python3 util/style.py src/arch/x86/vmcs.test.cc \
    src/arch/x86/vmx_utils.hh src/arch/x86/vmx_utils.test.cc
```

The recorded VMCS unit binary ran 9 tests and the validation/helper binary ran
7.  The transition integration reported 10 PASS markers (nine architectural
groups plus completion).  Normal smoke, active-root checkpoint creation, and
restore each passed every instruction/state check.  The optimized and fast X86
gem5 targets built successfully.  The recorded build emitted existing or
environmental warnings: GCC 14.3 was just beyond gem5's declared 14.2 support
ceiling, and the cached configuration did not enable optional Capstone, PNG,
or HDF5 support.  Linux 5.4 module preparation also warned that stack
validation was disabled; the modules were deliberately built with the
frame-pointer unwinder.  No VMX test failed in that recorded run.

Real-hardware differential tests were not run because a dedicated Intel bare
metal host was not available.  Non-root checkpoint/restore, compatibility
mode, asynchronous interrupts/NMIs, all individual VMX memory-fault priority
cases, and every dormant exit hook therefore remain unverified behavior and
are outside the research claim.

## Remaining unsupported behavior

- more than one logical processor, active-VMCS cross-CPU ownership, and VMX
  migration between logical processors;
- legacy protected, legacy PAE (including PDPTE loading), real, virtual-8086,
  and unrestricted guests; IA-32e compatibility mode is not regression-tested;
- PCID, EPT, VPID, nested paging, INVVPID, INVEPT, VMFUNC, and nested VMX;
- APIC access/x2APIC virtualization, virtual interrupt delivery, posted
  interrupts, interrupt/NMI-window exits, preemption timer, and SMM monitor;
- VM-entry event injection and VM-entry/exit MSR load/store lists;
- I/O/MSR bitmaps, CR3 targets, TSC offset/scaling, PML, VMCS shadowing,
  enclave/CET/FRED/PT/PKRS state, and advanced exit controls;
- architecturally complete guest interruptibility (`STI`/`MOV SS` shadows),
  the VM-entry precondition while blocked by `MOV SS`, async interrupt/NMI
  tests, and triple-fault/shutdown behavior;
- root-operation semantics of baseline gem5's warning/no-op INVD, GETSEC, and
  XSETBV implementations (their non-root VM exits are modeled);
- general guest delivery of abort-class exceptions: bitmap interception is
  attempted before the baseline gem5 abort panic, but broad behavior is not
  claimed;
- VMX abort-indicator updates and machine-check semantics for failures while
  the simulator accesses VMXON/VMCS physical memory through `PortProxy`; and
- checkpoint/restore while actually executing in non-root operation and
  real-hardware differential results.

The capability bits for every item above are disabled or its enabling MSR is
unmapped.  Where no capability exists, entry validation requires dependent
fields/counts to be zero or the behavior is explicitly outside this contract.
