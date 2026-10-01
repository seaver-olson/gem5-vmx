# x86 paging prerequisites and EPT release gates

## Scope and baseline

Work began on 2026-09-14 from clean `vmx-ext` commit `cfda3cfabe`.
The EPT plan is accepted as a staged implementation contract, not as a claim
that EPT or timing VMX currently works. Production EPT advertisement must stay
disabled through the complete phase-6 acceptance matrix.

## Prerequisite status (2026-09-19)

The corrected no-EPT path is the baseline to preserve when EPT is added.
It intentionally differs from `cfda3cfabe` where that revision was incorrect.
A passing regression is evidence for its covered cases, not proof that every
x86 paging mode or interleaving is implemented.

1. **MMU ownership and stage contracts — implemented and validated:** address
   checks, permission policy, synthetic address spaces, physical finalization
   and CPU completion now live in `MMU`. The walker returns a typed guest-stage
   result; the MMU checks its generation, publishes the cache entry, and owns
   continuation/drain accounting. Cache lookup/replacement and statistics stay
   in the TLB. All 56 no-EPT configurations retain identical measured ticks,
   instruction/operation counts and TLB access/miss counts across this refactor.
2. **Shared physical transport — implemented and validated:** `PagingPort`
   owns request queuing, retries, callback routing and outstanding ownership.
   Independent clients submit directly through the originating I/D endpoint,
   without entering the guest admission queue. The original two external ports
   remain. Unissued obsolete updates are cancelled; issued operations retain
   ownership until their responses arrive.
3. **Current guest context/lifecycle contract — implemented and validated:**
   the MMU captures an immutable context shared with the walker and continuation.
   It includes paging registers, retained PDPTEs, permissions, request flags,
   normalized/fault addresses, generation, captured EPT configuration, APIC
   state and originating port. No asynchronous completion rereads these from the
   CPU. Targeted tests cover queued, rejected and issued reads/updates, callback
   reentry, squash batches exceeding the per-cycle limit, concurrent I/D work,
   functional overlap and drain ownership. EPT-specific child continuations,
   generations and invalidation barriers remain part of EPT implementation;
   randomized multiprocessor stress and broader workloads remain extra coverage.
4. **Attributes and supported-mode limits — deferred until EPT progress:** carry raw PWT/PCD/PAT information
   and implement the specified memory-type resolution before EPT release.
   Full PAT/MTRR fidelity is not established. Legacy PAE #GP/#PF results are
   tested directly, but CPU delivery of legacy exceptions is still missing;
   implementing that requires separate exception-delivery work. Guest 1 GiB
   pages and LA57 remain separate, unadvertised extensions.
5. **Separate VMX prerequisite — deferred until EPT progress:** replace synchronous VMX memory operands
   with native timing-capable memory micro-ops and verify split operands,
   fault ordering and O3 dependencies. The MOV CR PDPTE loads do not complete
   this VMX instruction work.
6. **Reproducible no-EPT reference — recorded:** retain source/configuration,
   architectural test outcomes, simulated ticks and TLB counts. Compare each
   later structural change and EPT-disabled run against that reference;
   review deliberate changes instead of silently updating expected results.
   Hardware timing calibration and original-revision performance comparisons
   remain separate experiments.

## Compatibility with prior research

These prerequisites change architecturally visible results: permission and
reserved-bit faults, A/D state in guest memory, supported leaf sizes, context
isolation, functional translation, and control-register validation. They also
change simulated work and timing: coherent A/D operations complete before a
walk publishes, a first write may rewalk a clean cached translation, and
invalidation can cancel outstanding walks and squash prefetched instructions.
The three standard Classic hierarchies now include walker caches, changing
their topology and cache behavior. Restored TLBs start empty.
Legacy PAE now retains four PDPTE registers: ordinary walks no longer fetch
the PDPT from memory. Qualifying MOV CR operations load all four entries
through native memory micro-ops before validating and installing them. The
MOV CR expansion also adds micro-ops when a reload is unnecessary. Neither
control-register latency nor these performance effects have been calibrated.

Validation so far measures correctness, not before/after performance. No
quantitative performance equivalence or speedup claim is supported. Previous
research remains attributable to its original revision/configuration; paging-
sensitive experiments must be rerun before comparing against these repairs.
Keep the source revision, CPU/memory configuration, walker-cache topology,
checkpoint provenance, and warmup policy with published measurements.

## Architectural references

The architectural source actually consulted for the initial paging repairs is
[Intel SDM Volume 3A, 253668-085US, October 2024](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf),
chapter 5 (paging), particularly 5.3–5.8 and 5.10. Its numbering differs from
older editions. The June 2026 Volume 3C references in the proposed EPT plan
still need edition-specific verification before implementing those rules.
Legacy PAE PDPTE loading follows §5.4.1 and tables 5-7/5-8: present PDPTEs
reserve bits 2:1, 8:5 and 63:MAXPHYADDR, including NX even with NXE set.
Nonpresent entries ignore the other bits in this implementation, as permitted.
MOV CR validation additionally uses the instruction entry in
[Intel SDM Volume 2B, 253667-085US, October 2024](https://cdrdv2-public.intel.com/835752/253667-sdm-vol-2b.pdf).

Baseline evidence collected in this workspace:

- Existing `vmx_utils.test.opt`: 11 passing tests.
- Existing `vmcs.test.opt`: 9 passing tests.
- New disk-free CPU/walker regression against the unchanged gem5 binary:
  failure 50, NX at PML4 after a data fill (cold fetch already faults).
- Linux kernel, disk image, and module source resources exist locally.
  `/dev/kvm` is absent. Historical Linux/VMX runs are not new evidence.

## Findings checked against the baseline source

“Confirmed” below means source inspection, not that all required runtime
cases have passed. Validation and remaining work are separate sections.

| ID | Finding | Assessment and important qualification |
|---|---|---|
| 1 | NX ignored on TLB hits | Confirmed; reproduced with a shared I/D TLB. With separate TLBs, a data fill does not directly populate the ITLB. |
| 2 | NX not folded across levels | Confirmed; also missing in the cached legacy PAE result. |
| 3 | Terminal A update lost | Confirmed; `endWalk()` deletes the leaf read before write construction. |
| 4 | D never set, including warm first write | Confirmed; no cached dirty state or dirty update exists. |
| 5 | Descriptor update clobbers concurrent changes | Confirmed; ordinary whole-descriptor WriteReq from an old read. |
| 6 | Reserved bits not validated systematically | Confirmed. Legacy PAE PDPTE loading is additionally an architectural register-load problem: malformed PDPTEs loaded by MOV CR need #GP, not a walk-time #PF. |
| 7 | Long 4 KiB PAT uses bit 12 | Confirmed; correct selector is bit 7. |
| 8 | Legacy 4 MiB cached as 2 MiB | Confirmed; logBytes must be 22. |
| 9 | Legacy PTE overwrites PDE permissions | Confirmed. |
| 10 | PCID packed into excluded trie bits | Confirmed; may also trigger insertion assertions. |
| 11 | Invalidation loses context | Confirmed. Broad CR3 flushes can be conservative and permitted; they do not themselves establish an architectural failure. PCID advertisement and MOV CR operand validation need their own audit. |
| 12 | Functional bypasses checks/finalization | Confirmed for full-system translation. SE stack growth is existing intentional behavior to preserve. |
| 13 | GDB mutates VA, skips range interior | Confirmed; even an unchanged endpoint pair cannot validate inaccessible interior pages. |
| 14 | Live context at queued start/completion | Confirmed; acceptance-time capture is needed. |
| 15 | In-flight fills outlive invalidation | Confirmed; generations alone cannot undo already-issued writes. |
| 16 | Completion re-enters TLB with null continuation | Confirmed architectural limitation. |
| 17 | No explicit walker drain accounting | Confirmed. Also found write-response leaks, absent queued packet cleanup, and a squash-budget scheduling defect. |
| 18 | Generic demapPage cannot carry TC | Confirmed; an explicitly called x86 entry point can solve architectural callers without pretending to override a nonvirtual function. |
| 19 | TLB owns orchestration | Confirmed in the baseline; orchestration now lives in MMU, with typed walker results and MMU-owned CPU completion. |
| 20 | Guest 1 GiB absent | Confirmed; LongPDP always descends. Default CPUID's 1-GiB-page bit is clear. |
| 21 | Guest LA57 absent | Confirmed; default CPUID's LA57 bit is clear. |
| 22 | Memory typing incomplete | Confirmed; correcting PAT extraction does not implement PAT/MTRR resolution. |
| 23 | Two external walker endpoints | Confirmed configuration constraint; preserve both interfaces during EPT work. |
| 24 | PSE enabled, non-large PDE uses wrong PT index | Newly found: PSEPD uses `norml2` instead of `norml1`. |
| 25 | Fault helper mutates callback access mode | Newly found: execute with NXE clear changes `mode` to Read. Construct error-code mode separately. |
| 26 | Canonical address validation missing | Confirmed in ordinary TLB translation. Check before walks, including request-end overflow and SS fault selection. |
| 27 | Paging-control changes retain prefetched instructions on O3 | MOV CR3/CR4 need the squash-after behavior already used by MOV CR0. Cold NX fetches reproduced the failure. |
| 28 | INVLPG executes ahead of older accesses on O3 | Reproduced as repeated cancellation of the preceding page-table write. Invalidation must be non-speculative and discard old-context fetches. |
| 29 | Instruction #PF reports aligned fetch-buffer address | O3 fetches buffers; CR2 must identify the faulting instruction byte (or the failing fragment of a split instruction). Reproduced with fetch at VA+8. |
| 30 | Cache-clean walk changes callback mode | A timing miss checks permissions as a read but must complete the original write-mode callback. The walker now retains both modes. |
| 31 | Cached fetch #PF ignores NXE/PAE error-code rules | Fetch classification must be clear when the supported paging mode does not enable NX. Direct MMU regression added. |
| 32 | RMW read-side faults may be classified as reads | Requests requiring write permission now report write faults for nonpresence and user restrictions too. An optional Intel-host probe confirms write classification for ordinary and locked RMW operations. |
| 33 | Retry may issue an obsolete update | Generation checks on responses did not prevent a previously rejected descriptor update from being issued on retry. Cancel such unissued work before forwarding it. |
| 34 | Shared I/D walker port transferred twice on takeover | Reproduced an assertion in generic MMU takeover. Transfer a shared port and shared TLB once; check that old/new sharing topology agrees. |
| 35 | MOV CR validation omits CR3 width and PCIDE prerequisites | Reproduced enabling PCIDE with CPUID.PCID clear (guest failure 160). Validate physical width and PCIDE transitions; consume the no-flush operand bit without storing it in CR3. |
| 36 | MOV CR0 permits clearing PG in 64-bit code | Confirmed in the consistency checks. This requires #GP; leaving IA-32e paging must first switch to compatibility mode. |
| 37 | SE process IDs are truncated to PCIDs | Reproduced incorrect data in two O3 SMT processes with PIDs 100 and 4196. Cache tags now separate logical-thread identity from the complete address-space identifier; FS uses PCID, SE uses the page-table PID. |
| 38 | Reentrant completion can retire an active walk as queued work | Reproduced after a rejected request's completion immediately starts another walk. The queue wrapper dropped active response ownership and completed cancellation early. Only unstarted work may retire in the wrapper; active work retires at its response or retry. |

## Implementation boundaries

The initial prerequisite patch repairs permission accumulation, cached NX,
leaf A and D, compare-and-OR descriptor updates, supported leaf layout,
physical-width validation, full-system functional checks, debugger range
iteration, PCID prefix separation, acceptance-time paging-register capture,
obsolete-fill rejection, and walker drain/packet cleanup.

The coordinator now resides in `mmu.hh/.cc`. TLB BaseTLB translation methods
are compatibility forwarders; CPU translation, permission checks, synthetic
MSR/PIO spaces, and APIC/m5op physical finalization belong to the MMU. Walkers
return a result containing the guest mapping, captured cache context and
generation. They neither publish translations nor complete CPU callbacks.
An MMU continuation owns the request, access/completion modes and cache target;
its lifetime contributes to MMU drain accounting. This is the single-stage
coordinator foundation, not an implementation of nested EPT continuations.

Descriptor updates use little-endian SwapReq atomic functors with returned
old data. Each required update completes before the next descriptor read or
successful publication. A changed descriptor restarts the walk. Functional
walks have independent state and perform no writes or cache fills.

Classic cache coherence imposes a transport constraint: a raw SwapReq cannot
snoop peer CPU caches (`Cache::handleSnoop` rejects its writable snoop).
The three standard Classic hierarchies now put an 8 KiB MMU cache in front of
each x86 walker port. Configurations that already provide walker caches keep
their existing path. This preserves the two external endpoints but changes
topology and performance, and old checkpoints need an explicit migration.
Custom cached Classic configurations must also provide a cache that acquires
ownership before the update reaches a coherent bus. Ruby uses its sequencer.

Each logical-thread/address-space tag gets a separate trie. Global entries may conservatively miss
across PCIDs. CR3 flushing remains conservative. Architectural INVLPG and #PF
call an explicit x86 context-aware entry point. Restore and takeover discard
translation caches. A per-TLB generation rejects obsolete fills; outstanding
update responses retain ownership until retirement. This is not yet the
serializing INVEPT update barrier required for EPT.

Legacy PAE PDPTEs are architectural ISA state. MOV CR3 reloads all four while
PAE paging is active, even for the same value. MOV CR0/CR4 reload when the
resulting mode is legacy PAE and PG/CD/NW or PAE/PGE/PSE/SMEP respectively
changes; SMEP itself remains unsupported. CR0.WP changes and INVLPG do not
reload them. Physical loads use CR3[31:5], with normal atomic or timing CPU
memory execution. Validation precedes installation of the complete set and
the control-register write; a malformed present entry returns #GP(0).
Walks capture the retained values on acceptance and start at the selected
page directory. Functional walks neither reload nor modify the PDPTEs.
Checkpoint/restore and CPU takeover preserve the retained values, independent
of memory. Old checkpoints active in PAE without these values are rejected.
Legacy task switching, legacy exception delivery and legacy-PAE VM entry are
not added by this change.

## Requirement-to-test matrix

| Requirement | Test or required evidence | Status |
|---|---|---|
| NX at every supported IA-32e level, cold and read-fill fetch | `tests/gem5/x86_paging`, codes 10–63, 4 KiB/2 MiB | Baseline failure 50 reproduced; all three CPUs pass Classic; timing/O3 pass Ruby |
| Leaf and nonleaf A, cold/warm D | Same test, codes 70–89 | CPU/memory matrix passes |
| Width and large-page reserved #PF | Same test, codes 100–120 | CPU/memory matrix passes |
| PDE write restriction; denied write leaves D clear | Same test, codes 121–122 | CPU/memory matrix passes |
| Warm supervisor write permissions after CR0.WP changes | Same test, codes 200–207, 4 KiB/2 MiB | CPU/memory matrix passes without a CR3 reload |
| Conditional update race and replacement | `paging.test`; real-port fixture replaces PFN or permissions before update; two CPU walkers race leaf A/D | Helper tests, deterministic replacement and coherent two-walker comparison-failure cases pass on Classic and timing Ruby; randomized multiprocessor stress remains open |
| PAT extraction, non-PAE/PSE/PAE translation | Mode-switch guests and direct MMU fixture | Supported leaves, PAT/address independence, permissions and reserved faults pass |
| Legacy PAE PDPTE loading/validation/retention | `pae.elf`, two helper tests, real ISA installation and MMU tests | AtomicSimple/TimingSimple/O3 Classic and timing/O3 Ruby pass; divergent-state checkpoint/restore and all three takeover pairs pass; legacy #GP delivery remains unsupported |
| Functional equality/no side effects/GDB range | Nonidentity CPU guest; functional calls during timing traffic; real GDB packets | Functional checks pass, including paging disabled; GDB nonidentity, split, interior-hole, overflow and no-A/D checks pass on AtomicSimple/TimingSimple/O3 Classic |
| PCID, globals, INVLPG, MOV CR policies | Same VA/different roots, large pages, no-flush operand | PCID tag isolation, INVLPG, CR3 width, PCIDE transitions, no-flush operand and PGE invalidation pass; no assumptions about retaining stale entries |
| Timing context/invalidation/drain | Forced retry, invalidation during read/update, queued context, takeover | Real-port lifecycle cases and three CPU switch pairs pass; exhaustive drain/squash interleavings remain open |
| VMX memory operands timing and split faults | AtomicSimple, TimingSimple, O3 instruction regression | Pending; no VMX operand changes in prerequisite paging patch |
| Root and SE compatibility | Linux transition and SE workloads | Transition assertions and TimingSimple SE hello pass; broad workload coverage remains open |
| Ruby update transport | MESI Two Level timing, retries, deterministic descriptor replacement and two CPU walkers | Real-port fixture passes, including independent coherent walker endpoints; randomized multiprocessor stress remains open |

### Runtime evidence collected on 2026-09-16

The expanded disk-free regression passed on AtomicSimple and TimingSimple,
both directly connected to SimpleMemory and with coherent Classic caches.
TimingSimple also passed with Ruby MESI Two Level and each standard Classic
hierarchy (`l1`, `private-l2`, `shared-l2`). These runs include NX, A/D,
reserved faults, functional nonidentity accesses without A/D effects, and
INVLPG remapping. O3 exposed findings 27–29; after their repairs the complete
39-case matrix passes (`tests/gem5/x86_paging/run.py`). This includes all three
CPUs on all three standard Classic hierarchies and O3 on Ruby MESI Two Level.
Results and commands are retained in `m5out-paging-matrix/results.json`.
The three optional PCID-profile cases exercise PCIDE prerequisites and the
CR3 no-flush operand. Default CPUID advertisement is unchanged. The CPU
guests also verify CR3 reserved bits, clearing PG from 64-bit code, global
invalidation, and WP changes on cached translations.
On 2026-09-17, the matrix also passed the new O3 SE SMT regression with
colliding low process-ID bits. Both processes failed before the context-tag
repair; both now report their own expected results. Direct cache tests cover
logical-thread isolation and scoped global invalidation for 4 KiB, 2 MiB,
and 4 MiB entries. These are correctness checks, not performance measurements.

The legacy PAE extension subsequently passed the expanded 52-configuration
matrix, including AtomicSimple, TimingSimple and O3 with cached/uncached
Classic memory and TimingSimple/O3 with Ruby MESI Two Level. Checkpoint,
restore and all three CPU-switch pairs retain PDPTE values when their backing
memory already differs. Seven helper tests pass, including reserved bits at
32/36/48/52-bit physical widths and the exact PDPTE reload conditions.

The separate `legacy.elf` tests PSE non-large indexing, 4 MiB offsets above
2 MiB, and legacy 4 KiB/4 MiB A/D updates. It passes on all three CPUs with
and without Classic caches, and timing/O3 Ruby. Negative legacy guest tests
encountered a pre-existing limitation: `deliverInterruptOrException` panics
for legacy exceptions with error codes. The direct MMU fixture inspects these
faults without invoking architectural delivery: 132 atomic and 143 timing
checks pass after PDPTE loading was added. These include permissions,
reserved encodings, PAT, functional
calls during timing traffic, replacement races, retries, and queued contexts.
The timing cases include queued absent PAE PDPTE completion and a reproduced
reentrant cancellation bug: cancellation must retain response ownership and
drain accounting until the response arrives. Both Classic and Ruby pass.

The separate GDB regression passes on AtomicSimple, TimingSimple and O3 with
Classic caches. It uses the real remote protocol and verifies nonidentity
translations, a successful split-page read, an inaccessible second or interior
page, overflowing ranges, and no page-table A/D effects. Commands and logs are
reproducible with `tests/gem5/x86_paging/gdb.py` and are required in CI.

The two-CPU fixture adds seven deterministic races covering supported 4 KiB,
2 MiB and 4 MiB leaves in IA-32e, PAE, non-PAE and PSE modes. The first walker
observes a clean leaf; its update is held while the second CPU's walker sets
A/D through a separate coherent endpoint. The first update must return a
comparison failure, rewalk, and preserve both D and a software-owned bit.
The fixture passes all 150 timing cases on Classic L1, private L2, shared L2
and Ruby MESI Two Level. The expanded runner passes all 56 configurations;
the three GDB CPU configurations pass separately.
This tests real independent walker transport, not concurrent guest instruction
streams or randomized multiprocessor stress.

TimingSimple with Classic caches passed drain/checkpoint at tick 6025000,
continued successfully, and passed independent restore with empty TLBs.
Atomic-to-Timing, Timing-to-O3, and O3-to-Atomic takeover also pass with Classic
caches. This does not establish every retry/update/squash/drain interleaving.
EFER.NXE transitions pass directly after WRMSR without an extra CR3 flush;
the existing ThreadContext write path supplies the necessary O3 squash.

The Linux `vmx_transition.ko` regression reached `PASS: COMPLETE` from the
local boot checkpoint, including a rerun after the MOV CR validation repairs
(`m5out-vmx-paging-cr-regression/board.pc.com_1.device`). A further run after
the PAE MOV CR expansion also reaches `PASS: COMPLETE`
(`m5out-vmx-pae-regression/board.pc.com_1.device`). For all these runs,
a copy of the checkpoint was
extended with empty walker-cache sections to match the new hierarchy; the
original checkpoint was untouched. The console also reports two userspace
service crashes, so this is evidence for the transition assertions, not a
clean whole-system regression. Existing VMCS (9) and VMX utility (11) unit
tests pass. The existing x86 SE hello workload passes on TimingSimple with
caches. This is a smoke test, not broad SE workload coverage.

### MMU refactor/no-EPT comparison on 2026-09-18

All 56 configurations pass again in `m5out-paging-mmu/results.json`, including
checks that MMU continuations remain visible to drain during functional and
reentrant access and disappear after retirement. Compared with the preserved
pre-refactor results in `m5out-paging-pre-mmu`, simulated ticks, emitted
instruction/operation counts, and every selected I/D TLB access/miss count
match exactly. These are measurements of this structural refactor against
the already repaired paging path, not against the original buggy revision
or against hardware. Host runtime is excluded.

All three GDB CPU configurations pass after the move, and the Linux VMX
transition regression again reports `PASS: COMPLETE`
(`m5out-vmx-mmu-regression/board.pc.com_1.device`).
`tests/gem5/x86_paging/baseline.py` records and compares the signatures; its
separate `capture` command records source revision/working-tree digest and
binary hashes before execution. Behavioral tests do not require capture. The
comparison rejects a deliberately injected one-tick difference. CI retains
the signature and provenance for later no-EPT comparisons. The old results
predate source-manifest recording and are explicitly marked as such.

## Shared transport and immutable context validation

`paging_port.hh/.cc` adds a `QueuedRequestPort` with a free-port immediate
send path, a shared FIFO for backpressure, per-packet validity checks, and
explicit cancellation/response callbacks. Reentrant submissions cannot bypass
an older rejected packet. Ownership includes the callback itself. The walker
accounts for all clients on its endpoint when draining, including clients
outside its guest admission queue. Generic packet queues account for scheduled
sends; MMU continuations account for original CPU requests. No new external
port or crossbar was introduced.

`translation.hh` defines the immutable guest context. The MMU captures it
before dispatch, after synchronous linear/segment checks. Walker admission,
compare-failure restarts, permissions, fault construction and final APIC
resolution use that same snapshot. TLB-hit and non-walking paths remain
synchronous. Functional walks have independent per-call state and do not
populate caches or update A/D bits. This is the current guest-stage contract;
EPT contexts and barriers will extend it when their rules are implemented.

Evidence from the final build:

- All 56 existing configurations pass in `m5out-paging-transport-baseline`.
  Compared with `m5out-paging-mmu/signature.json`, all selected simulated ticks,
  instruction/operation counts and I/D TLB access/miss counts match exactly.
- The separate transport matrix passes all six memory configurations in
  `m5out-paging-transport-extended`: uncached/cached Classic (149 fixture cases
  each), and Ruby MESI Two Level plus the three standard Classic hierarchies
  (156 each, including peer-walker races). Its six additional cases exercise
  shared clients, cancelled conditional updates, reentrant callbacks,
  functional traffic with queued requests, captured WP/CPL/CR3/CR4/NXE/APIC
  and request flags, concurrent I/D walks, and ten-request squash batches at
  rejected/issued read/update boundaries. An issued update may finish; a
  rejected update must not reach memory, and neither may publish a stale fill.
- Real GDB protocol tests pass on AtomicSimple, TimingSimple and O3 in
  `m5out-paging-transport-gdb-*`.
- The restored Linux/AtomicSimple VMX transition smoke test reports
  `PASS: COMPLETE` in `m5out-vmx-transport-regression/board.pc.com_1.device`.
  This is transition coverage, not a timing/O3 VMX execution claim.
- `m5out-paging-transport-baseline/signature.json` is the new preserved no-EPT
  reference. Its provenance includes simulator/guest hashes and a hashed
  `source.tar.gz` containing HEAD, tracked changes and untracked source files.
  The source archive hash and patch application to the recorded HEAD were
  checked. CI retains the archive and separately runs the transport matrix.

These measurements establish equivalence for the covered no-EPT workloads,
not a general performance or hardware-fidelity claim. The forced context
mutation tests intentionally check corrected behavior, particularly captured
APIC finalization and request permission flags. Real nested EPT timing,
INVEPT update barriers and event-delivery failures are still release gates.

## EPT phase tracking

### Paging change inventory

This inventory describes the cumulative paging series, including the legacy
PAE work. It does not claim the EPT prerequisites or EPT release gates below
are all complete.

| Files under `src/` unless noted | Implemented change |
|---|---|
| `arch/x86/pagetable_walker.hh/.cc` | Accumulate U/S, R/W and NX; retain leaf A updates; set D on permitted writes; validate supported reserved encodings, physical widths and large-page alignment; correct PAT, PSE indexing and 4 MiB size; use retained legacy PAE PDPTEs; capture context on acceptance; return explicit completion results; retire packet ownership during invalidation, retries and drain. |
| `arch/x86/paging_port.hh/.cc`, `translation.hh` | Shared queued physical transport, validity-checked retry/cancellation, per-client response routing and callback ownership; immutable MMU-captured guest context including originating port, EPT configuration and APIC finalization state. |
| `arch/x86/paging.hh` | Pure PDPTE validation/reload rules, physical-width and fetch-fault-address helpers, and little-endian conditional compare-and-OR descriptor updates. |
| `arch/x86/tlb.hh/.cc` | Separate context tags from address prefixes; cache lookup/LRU/replacement and statistics; invalidation generations; empty caches on restore/takeover. Legacy BaseTLB translation entrypoints forward to MMU. |
| `arch/x86/pagetable.hh/.cc` | Store dirty state and structured logical-thread/address-space tags, with serialization support. |
| `arch/x86/isa.hh/.cc` | Retain/reset/copy/checkpoint four PAE PDPTEs; validate before installing any; reject incomplete active-PAE checkpoints; flush on NXE/PCIDE changes. |
| `arch/x86/isa/insts/general_purpose/data_transfer/move.py`, `isa/microops/regop.isa`, `isa/operands.isa`, `isa/includes.isa` | Native MOV CR PDPTE load/validation sequence and register dependencies; CR3 physical-width/no-flush handling; PCIDE prerequisites; CR0 consistency checks; squash old-context instruction fetches after paging-control writes. |
| `arch/x86/isa/microops/ldstop.isa` | Physical, non-speculative PDPTE loads with real timing execution; serialize INVLPG and route it through context-aware invalidation. |
| `arch/x86/mmu.hh/.cc`, `arch/x86/X86MMU.py`, `arch/x86/faults.cc` | MMU-owned atomic/timing/functional translation, cached permissions, first-dirty rewalk policy, synthetic spaces, physical finalization, typed walk continuations and drain accounting; explicit context-aware linear invalidation for INVLPG and page-fault delivery. Physical requests retain their HPA interpretation. |
| `arch/x86/remote_gdb.cc` | Validate every page of the original virtual range through functional MMU translation; reject range overflow. |
| `arch/x86/process.cc` | Clarify that SE translation tags use the full process ID rather than a truncated PCID. |
| `arch/generic/mmu.cc` | Transfer a shared I/D walker port and TLB once on CPU takeover; check matching sharing topology. |
| `python/gem5/components/cachehierarchies/classic/walker_ports.py` and the three private-L1 hierarchy modules; `python/SConscript` | Supply an 8 KiB owner cache per x86 walker where needed for coherent atomic updates, preserving the two external endpoints. |
| `arch/x86/paging.test.cc`, `paging_tester.hh/.cc`, `X86PagingTester.py`, `arch/x86/SConscript` | Helper tests and real-port MMU fixture, including forced retries, replacement, context changes, functional overlap, PAE state and lifecycle checks. |
| `tests/gem5/x86_paging/`, `.github/workflows/vmx-tests.yaml` | Disk-free IA-32e, non-PAE/PSE and PAE guests; Classic/Ruby CPU matrix; checkpoint/takeover, SE SMT isolation, GDB and optional Intel-host probes; required correctness CI and retained logs. |

### Remaining EPT gates

All required configurations must pass, and evidence must name the tested CPU
and memory system. A build or a pure helper test cannot satisfy a port-level
or architectural execution requirement.

| Phase | Contract | Gate |
|---|---|---|
| 0 | Traceable requirements and reproducible baseline | `no-ept-baseline` branch and recorded 56-case signature; EPT-specific release requirements remain open |
| 1 | Guest paging repair; separately repair VMX memory operands | No-EPT paging and native VMX operand gates pass; do not enable EPT |
| 2 | MMU coordinator, typed context, queued shared ports, explicit stage results, drain | Current guest coordinator, immutable context, shared queued endpoints, explicit results and lifecycle tests pass with unchanged 56-case no-EPT measurements; EPT child lifecycle integration remains in phases 4–5 |
| 3 | EPTP/entry rules, LRU cache, 4/5 levels, 4 KiB/2 MiB/1 GiB, memory types | EPTP decoder, VM-entry validation, capability-gated VMCS fields and 4-/5-level entry-address planning added; packet walk/cache/leaf/memory-type work pending |
| 4 | EPT on every guest descriptor access/update and final GPA; VM entry/deferred faults | Pending |
| 5 | Nested timing, event vectoring, cancellation, checkpoints/takeover | Pending |
| 6 | INVEPT ordering/barriers, capabilities, Classic/Ruby and CPU release matrix | Pending; production EPT remains disabled |
| 7 | EPT A/D and separately specified extensions | Deferred |

The `ept-bootstrap` branch adds an EPTP decoder, VM-entry validation hook,
and an entry-time EPT configuration snapshot carried into each MMU
`TranslationContext`. It checks capability-supported UC/WB memory type,
4-/5-level walk length, A/D enable, reserved bits, and physical-address
width. EPTP now has its architectural 0x201A VMCS location, with VMREAD and
VMWRITE gated by the advertised secondary/EPT control capabilities. The
captured context can form a typed EPT lookup request that preserves the GPA,
access type and whether it came from a guest page-table access or the final
memory access. A pure 4-/5-level walk plan computes EPT entry addresses;
it does not yet fetch entries or grant access. EPT remains unadvertised and
VM entry requests for EPT are rejected until GPA-to-HPA translation and EPT
exit handling are implemented. Phase 3's packet walk, cache, leaf sizes,
and effective memory-type work remain open.

The target retains IA-32e guests and 64-bit hosts. EPT's walk depth and leaf
sizes do not enable guest LA57 or guest 1 GiB pages. VPID, unrestricted guests,
legacy-PAE VM entry, VMFUNC, PML and #VE remain unadvertised. EPT transport
must retain `mmu.itb.walker.port` and `mmu.dtb.walker.port`, and child EPT
walks must never queue behind the blocked guest parent.

EPT acceptance still requires distinct GVA/GPA/HPA mappings, all leaf-size
intersections, deeper misconfiguration after upper permission denial,
permission/fault priority and exact origin metadata, remapping/INVEPT across
EPTP configuration variants, split operands, vectoring failures, physical
finalization, no functional A/D effects, and warm-state checkpoint/takeover.
Required CI cannot depend on the best-effort Linux boot job. Publication must
separate architectural correctness, validated configurations, and memory-type
performance fidelity.
