# Disk-free x86 paging regression

This freestanding ELF runs privileged paging tests through actual CPU and
walker ports. It needs a host C compiler and gem5, but no Linux image, KVM,
network download, or guest module build.

Run the CPU/memory/checkpoint/takeover matrix with
`python3 tests/gem5/x86_paging/run.py`. It retains commands, statuses, logs,
and simulator statistics under `m5out-paging-matrix`. CI runs this job without
`continue-on-error`; it is independent of the Linux boot job.

The matrix has 58 cases: the original 56 plus cached and uncached MinorCPU
IA-32e paging runs. Those Minor cases include cold/warm NX at each level,
nonpresent and reserved-entry fetch faults, CR2/error-code checks, and return
from the real exception handler. They do not add Minor Ruby or legacy-mode
coverage. Regenerate measurement baselines for this test set; the ordered
load before the host-side functional export also changes guest counters.

For a reproducible no-EPT baseline, build the simulator and guests, capture
provenance into a fresh output directory, run the matrix, and record its
signature before further architectural changes:

```
make -C tests/gem5/x86_paging
python3 tests/gem5/x86_paging/baseline.py capture --results-dir m5out-paging-matrix
python3 tests/gem5/x86_paging/run.py --outdir m5out-paging-matrix
python3 tests/gem5/x86_paging/baseline.py record --results-dir m5out-paging-matrix --baseline m5out-paging-baseline.json
python3 tests/gem5/x86_paging/baseline.py capture --results-dir m5out-paging-candidate
python3 tests/gem5/x86_paging/run.py --outdir m5out-paging-candidate
python3 tests/gem5/x86_paging/baseline.py compare --results-dir m5out-paging-candidate --baseline m5out-paging-baseline.json
```

The comparison requires the same passing test set and exact simulated tick,
instruction/operation (where emitted), and TLB access/miss counts. Host speed
is excluded. The separate `capture` command records the source revision,
working-tree digest and simulator/guest binary hashes. Run it after building
and immediately before executing tests, without changing the source or
binaries between capture and execution. `source.tar.gz` retains `HEAD`, the
binary-capable tracked patch and
untracked source files (including executable modes). To reconstruct the source,
check out the recorded HEAD in a separate checkout, apply `tracked.patch`, and
copy the archive's `untracked/` contents into that checkout. Preserve this
archive with `provenance.json`, configuration files, logs and statistics.
Behavioral tests run without Git metadata or provenance capture. CI captures
provenance in a separate step before the tests and checks capture succeeded
before recording the signature, after the behavioral regressions have run.
`capture` refuses to overwrite existing evidence. A signature recorded without
capture explicitly marks its source provenance as missing.
If a container cannot access checkout Git metadata, CI instead records
`source_state: github-sha-only; worktree changes unverified` and archives only
the GitHub checkout SHA. That evidence identifies the requested revision but
does not prove that the built working tree was unchanged; do not use it as a
fully reconstructible source snapshot.
This checks behavior
against the corrected simulator baseline, not timing fidelity against hardware.
Intentional bug fixes may require a reviewed new baseline. Future EPT-disabled
runs should preserve it; EPT-enabled runs will have their own expected costs.

```
make -C tests/gem5/x86_paging
scons build/X86/gem5.opt build/X86/arch/x86/paging.test.opt -j6
build/X86/arch/x86/paging.test.opt
build/X86/gem5.opt --outdir=m5out-paging-atomic tests/gem5/x86_paging/config.py --cpu atomic
build/X86/gem5.opt --outdir=m5out-paging-timing tests/gem5/x86_paging/config.py --cpu timing
build/X86/gem5.opt --outdir=m5out-paging-o3 tests/gem5/x86_paging/config.py --cpu o3
```

A complete pass reports `PAGING RESULT: m5_fail instruction encountered code=0`.
The fail pseudo-operation is deliberately used to transmit a numeric result;
the Python harness rejects every nonzero result and simulation timeout.
A shared I/D TLB ensures data fills test cached execute permissions. Memory
requests use a Classic crossbar and SimpleMemory. Add `--caches` to exercise
coherent CPU and walker caches. The standard-library configurations use
independent I/D TLBs:

```
build/X86/gem5.opt --outdir=m5out-paging-ruby tests/gem5/x86_paging/ruby.py --cpu timing
build/X86/gem5.opt --outdir=m5out-paging-classic tests/gem5/x86_paging/ruby.py --cpu o3 --classic private-l2
```

Ruby uses MESI Two Level and accepts `timing` or `o3`. Classic accepts all
three CPUs and `l1`, `private-l2`, or `shared-l2`. These are single-core tests;
they do not establish concurrent descriptor-replacement correctness.

Add `--guest legacy` to either configuration for 32-bit non-PAE/PSE positive
translation tests: 4 KiB indexing, 4 MiB upper-half offsets, and A/D updates.
Legacy error-code exception delivery is unimplemented in gem5, so this guest
does not claim negative permission/reserved-fault coverage.

`--guest pae` exercises retained legacy PAE PDPTE registers: all four slots,
same-value CR3 reloads, CR0.PG/CD/NW and CR4.PAE/PGE/PSE transitions, legal
ignored bits, and retaining the old root after its backing memory changes.
CR0.WP changes and INVLPG must not reload the registers. The runner also
checkpoints, restores, and switches CPUs at a marker where the retained values
deliberately differ from memory. Use `--guest pae --pae-checkpoint` to save at
that marker, or `--guest pae --switch-to o3 --caches` to switch there.
Invalid present PDPTEs are checked through the real ISA installation function
and pure validation tests, including validation of all four entries before
changing state. Architectural legacy #GP delivery remains unsupported, so
these checks inspect returned faults rather than executing their handlers.
Old checkpoints active in legacy PAE without retained PDPTE state are rejected.

`--fixture` instead suspends the CPU and drives its real MMU and walker ports.
It checks returned faults without invoking them, covering user/supervisor
restrictions in long, PAE, non-PAE, and PSE modes; PAT; functional no-side-effect
behavior; cache-clean callbacks; and PCID tags. A port interposer injects
descriptor replacements, retries, and invalidation during outstanding work.
Use `--cpu atomic` or `--cpu timing`; `ruby.py --cpu timing --fixture` selects
the Ruby transport. This fixture complements the CPU-executed guests.

`ruby.py --cpu timing --fixture-peers` adds a second CPU walker. The fixture
holds one walker's leaf update while the other walker sets A/D through its
own coherent port, then verifies that the old conditional update fails and
rewalks without losing dirty or software bits. Add `--classic l1`,
`--classic private-l2`, or `--classic shared-l2` for Classic coherence.

The shared transport and lifecycle extensions run separately from the
CPU/paging measurement set:

```
python3 tests/gem5/x86_paging/run.py --transport-only --outdir m5out-paging-transport
```

This runs uncached/cached Classic, the three standard Classic hierarchies,
and Ruby MESI Two Level. `--transport` on either configuration enables the
additional cases. They exercise independently admitted physical clients on
the same endpoint, callback reentry, cancelled conditional updates, functional
translation while requests are queued, acceptance-time register/request/APIC
snapshots, and ten-request squash batches at read/update rejection and response
boundaries. Standard-library fixtures have independent I/D walkers. Drain must
retain requests until their callbacks/responses retire. These tests exercise
the physical transport interface; actual nested EPT continuations remain EPT
implementation work.

The memory-backed EPT walker has an isolated atomic/functional/timing fixture:

```
python3 tests/gem5/x86_paging/run.py --ept-only --outdir /tmp/ept-walker-tests
```

It runs with and without a Classic walker cache, in atomic and timing system
configurations. Atomic configurations compare atomic and functional results
for all 109 scenarios (218 lookups); timing configurations replay the same
scenarios asynchronously and compare against their functional results.
Functional lookups must leave simulated time and table contents unchanged.
Tables are initialized by the fixture, then every EPT entry is read as an
eight-byte physical request through the existing `PagingPort` and memory
hierarchy. Tests cover four- and
five-level walks, 4 KiB/2 MiB/1 GiB leaves, permission accumulation, violation
versus misconfiguration ordering, leaf attributes, UC/WB requests, unchanged
tables, physical read errors, and exact accumulated atomic memory latency.
Invalid inputs, including EPT A/D without its capability bit, fail before
any memory request.
Returned HPAs deliberately lie outside fixture RAM to verify that the walker
does not access translated data. Additional timing cases exercise retries,
cancellation before admission, at rejection and after issue (including a
failed response), functional lookup while a timing read is queued, independent
EPT roots with different UC/WB attributes, and callback reentry. They verify
one outstanding read per lookup, exactly-once completion, unchanged tables,
response-driven completion time and release of callback-held state after
retirement. Cancellation of an issued read waits for its response; it does
not withdraw the packet from memory.

Focused EPT A/D cases enable EPTP bit 6 and issue real eight-byte conditional
updates at the entry HPAs through `PagingPort`. They check accessed bits at
every used valid level and dirty bits only on allowed leaf writes, including
large pages and five-level walks. Exact word comparisons check that addresses,
permissions, software and ignored bits survive. A changed entry fails its
compare and restarts at the captured root; replacement cases also revoke root
write permission to detect stale accumulated rights. Tests cover already-set
bits, absent/malformed/denied entries, failed updates, retry and cancellation.
An issued update can commit despite cancellation or an error response;
rejected cancelled updates cannot. Functional walks remain read-only.

The same suite also calls the public MMU atomic, functional and timing APIs
with a suspended CPU and a captured EPT configuration. Guest virtual addresses,
guest physical addresses and backing host physical addresses all differ.
Guest table pages use separate 4 KiB EPT mappings; the final data page uses
a 2 MiB EPT mapping. Successful translations are followed by real byte reads
or writes through `PagingPort`, including execute-mode instruction-byte reads.
Cold and warm I/D TLB cases verify that cached entries retain GPAs and still
apply EPT permissions and HPA remapping on each final access. Functional cases
must preserve guest A/D bits, EPT tables and simulated time.
With EPT A/D enabled, normal guest page-table reads require EPT write
permission and dirty their EPT leaf even when guest A/D bits are already set.
The tests check both read/write qualification bits on denial, combined guest
and EPT A/D updates, and a warm-TLB final write setting the EPT data leaf's D
bit without rewalking guest tables.

Fault cases inspect the VM-exit fault's reason, qualification, GPA and GLA,
including table reads and A/D writes at each guest paging level, a separate
dirty-bit update after a read-filled TLB entry, inherited EPT restrictions,
read-modify-write access, and guest page-fault priority. A replaced guest leaf
must fail its conditional update and rewalk without losing D or software bits.
Timing cases inject retry, squash and invalidation during nested table reads,
A/D lookups, conditional writes and final lookups on cold and warm TLB paths.
They check exactly-once completion and drain ownership, including cancellation
before admission and cancellation racing a failed memory response. An issued
conditional write may update memory before cancellation; a rejected write must
not. Retry cases also change the live EPT state and capabilities to verify that
in-flight work uses its captured context.

The suspended-CPU fixture seeds private EPT state and restores it and the
capability MSR after each test. It isolates MMU faults and lifecycle behavior;
it does not replace architectural VM entry/exit coverage.

The same `--ept-only` suite also runs `ept_entry.elf` on AtomicSimple,
TimingSimple, Minor and O3 with Classic caches. This is a second build of the
existing VM-entry guest, not a new fixture: real VMLAUNCH/VMRESUME captures
EPTP, real guest fetches/loads/stores use distinct GVAs/GPAs/HPAs, and VMREAD
checks exits 48/49, qualification, GPA, valid GLA and saved guest RIP. It
rejects an invalid EPTP before launch, repairs mappings and resumes faulting
instructions, checks guest paging A/D update failures, then verifies successful
VMCALL exits. It repeats with EPT A/D off/on and different roots, checks actual
guest/EPT A/D changes and untouched neighboring data, and finally disables EPT
on resume. No test-side capability override or private-state seeding is used
by this guest.

Entry/exit instructions squash younger work after the transition so O3 cannot
reuse bytes fetched with the previous host/guest translation context, even
when its branch-target prediction was correct. The existing VM-entry trace
also checks this flag without changing the configured execution latencies.
O3 discards its virtual-address-tagged byte buffer on traps and squash-after
redirects; ordinary branch redirects retain it. Refetch may change total cycle
counts even though the instruction's configured `opLat` is unchanged.

The advertised profile is deliberately limited to four-level WB EPT tables,
4 KiB EPT leaves and optional A/D updates, with IA-32e guests and 64-bit hosts.
INVEPT, VPID, five-level/large-page EPT advertisement, active-EPT checkpoints,
event-vectoring fault coverage and a Linux/KVM boot remain outside this patch.

The native VMX operand guest runs independently of VM entry/exit and EPT:

```
python3 tests/gem5/x86_paging/run.py --vmx-operands-only --outdir m5out-vmx-operands
```

It covers VMXON, VMCLEAR, VMPTRLD, VMPTRST, VMREAD and VMWRITE with actual
register/memory operands on AtomicSimple, TimingSimple and O3, plus timing/O3
Ruby MESI Two Level. Checks include indexed/RIP-relative addressing, high
64-bit field values, split-page success/faults, unchanged flags on operand
faults, unsupported/read-only fields, and precheck-versus-operand fault
priority. Advertised EPTP, secondary controls and guest-physical exit fields
are accessible even while EPT is disabled. This is operand execution coverage;
the separate entry guests cover VM entry/exit.

The focused VM entry regression adds a minimal no-EPT launch/resume/VMCALL
round trip, plus #UD, VMfailInvalid and VMfailValid checks for both entry
instructions:

```
python3 tests/gem5/x86_paging/vmresume.py --outdir /tmp/vmresume-tests
```

It checks architectural behavior on AtomicSimple, TimingSimple and MinorCPU,
then O3 with its production defaults and independent synthetic launch/resume
`opLat` values of 1, 21 and 41. The O3 trace checks both operation classes,
existing serialization/control flags, exact issue-to-execution intervals,
two successful launches and four successful resumes. The guest checks exit reason,
saved RIP and instruction length, and a guest-memory counter detects skipped
or repeated execution. A failed second launch must preserve launch state so
subsequent resumes still succeed. After the ordinary round trips, a second launch
injects #UD and a resume injects #GP with an error code. The handlers verify the
saved guest RIP, status flags and error code and exit directly through VMCALL;
the normal guest entry must not run before either handler. Clearing injection
then exercises ordinary resume again. This guards O3's executing-instruction
PC redirect as well as the shared ROM delivery path. These focused injection
checks do not cover IRET, all event types or EPT.

CI builds the disk-free guests before compiling gem5, so guest C/assembly
errors fail early rather than preventing every regression after a long build.

All four CPU models run the same guest, including the pre-VMXON #UD checks
for VMRESUME, VMLAUNCH, VMCALL and VMXOFF, plus a UD2 macro-op fault. Minor's
trace checks that each VMX opcode faults exactly once, guarding against
reexecuting it instead of dispatching exception-ROM microcode. The guest
checks the delivered vector, return to the probe and preserved status flags.
This is focused exception coverage, not a complete VMX exception suite.
Minor also dispatches ROM after an instruction-fetch #PF, without requiring
bytes from the faulting PC. A byte-free continuation uses the normal pipeline
buffers and backpressure; ROM completion redirects normal instruction fetch
to the handler. The handler's code/data accesses still use the real MMU and
memory ports. The two Minor paging matrix cases guard this path.

The configurable latencies target O3. Its default x86 `VmxResumeUnit` and
`VmxLaunchUnit` are separate non-pipelined units with **uncalibrated** one-cycle
`opLat` defaults. The two `opLat=1` lines marked `Skylake` in
`src/cpu/o3/FuncUnitConfig.py` are the calibration points. Replace those values
with calibrated execution costs in simulated CPU cycles, excluding measurement
overhead and costs already modeled by gem5. Do not change the synthetic test
values; the default-case checks read back the configured production latencies.

This config exposes `--vmresume-latency N` and `--vmlaunch-latency N` for
per-run O3 overrides. MinorCPU's default miscellaneous unit recognizes both
operation classes at its existing one-cycle latency; this is compatibility,
not a Skylake timing model. Custom functional-unit pools must support both
classes. AtomicSimple and TimingSimple do not use these O3 latencies.
Each opcode currently has one execution latency for all outcomes, including
failures; VMCALL timing is unchanged. These settings reuse O3's existing
scheduler. Hardware calibration and outcome-dependent timing remain future work.

The separate debugger test sends real GDB remote-protocol packets over a local
Unix socket. It checks nonidentity data, successful cross-page reads, a missing
second page, an inaccessible interior between mapped endpoints, overflowing
ranges, and unchanged A/D bits after debugger reads:

```
python3 tests/gem5/x86_paging/gdb.py --cpu timing
```

It also accepts `atomic` and `o3`. The required CI job runs all three.

An optional Linux/Intel host differential probe checks page-fault read/write
and fetch classification:

```
make -C tests/gem5/x86_paging host_faults
tests/gem5/x86_paging/host_faults
```

It skips non-Intel hosts (status 77) and does not require root. It deliberately
does not compare the present bit for `PROT_NONE`, whose PTE encoding is chosen
by the host kernel. This is limited fault-classification evidence, not a
hardware differential EPT suite.

To drain during execution, checkpoint, continue, and restore independently:

```
build/X86/gem5.opt --outdir=m5out-paging-drain tests/gem5/x86_paging/config.py --cpu timing --caches --checkpoint-at 6025000
build/X86/gem5.opt --outdir=m5out-paging-restore tests/gem5/x86_paging/config.py --cpu timing --caches --restore m5out-paging-drain/checkpoint
```

Use the same ELF and configuration for restore. New walker caches change the
SimObject graph of the three standard Classic configurations; an old checkpoint
without those caches is not directly interchangeable with the new topology.

The runner also switches AtomicSimple to TimingSimple, TimingSimple to O3,
and O3 to AtomicSimple. Use `--switch-to` and `--switch-at` with `config.py`
to select a switch explicitly. `--pcid` enables a test-only CPUID profile for
PCIDE transition and CR3 no-flush operand checks; the default profile is unchanged.

Failure codes: 10–23 cold NX; 30–43 NX data read; 50–63 warm NX; 70–79 read and
A; 80–89 D; 100–113 address width; 120 large-page alignment; 121–122 restrictive
PDE/write and D; 123–125 NXE/reserved and nonpresent priority; 126 canonicality;
130–134 functional nonidentity read/write with no A/D changes; 135 ordered
source load before functional export; 140–143 INVLPG
permission and PFN replacement; 150–151 EFER.NXE changes; 160 unsupported
PCIDE; 161–176 CR3 reserved address bits; 177 ignored low bits; 178–181 PCIDE
transition and no-flush operand semantics; 182 clearing PG in 64-bit code;
190–195 global-page invalidation through CR3, INVLPG, and CR4.PGE; 200–207
CR0.WP changes on warm translations. Code 255 means an unexpected exception.

Successful runs also verify the bytes in `functional.bin`. Failed checks write
`failure.bin` containing seven little-endian 64-bit words: check number, fault
error code, vector, PML4E, PDPTE, PDE, and PTE. Checkpoints and traces belong in
the output directory, not in the source tree.

The baseline at `cfda3cfabe` fails code 50. See
[the audit and remaining release gates](../../../docs/x86-paging-ept-status.md)
for coverage limits. This is a prerequisite paging suite; it does not claim
EPT coverage.

`se.py` runs two O3 SMT processes whose IDs differ by 4096 and whose ELF
virtual layouts match. Each must observe its own code/data and produce its
own expected output. The pre-repair cache reduced both IDs to the same PCID;
the regression reproduced incorrect data in both processes. This is SE SMT
coverage; full-system O3 currently rejects multiple hardware threads.
