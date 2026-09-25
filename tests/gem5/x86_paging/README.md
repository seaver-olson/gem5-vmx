# Disk-free x86 paging regression

This freestanding ELF runs privileged paging tests through actual CPU and
walker ports. It needs a host C compiler and gem5, but no Linux image, KVM,
network download, or guest module build.

Run the CPU/memory/checkpoint/takeover matrix with
`python3 tests/gem5/x86_paging/run.py`. It retains commands, statuses, logs,
and simulator statistics under `m5out-paging-matrix`. CI runs this job without
`continue-on-error`; it is independent of the Linux boot job.

For a corrected no-EPT baseline, keep the results directory from a passing
revision and record its signature before further architectural changes:

```
python3 tests/gem5/x86_paging/baseline.py record --results-dir m5out-paging-matrix --baseline m5out-paging-baseline.json
python3 tests/gem5/x86_paging/run.py --outdir m5out-paging-candidate
python3 tests/gem5/x86_paging/baseline.py compare --results-dir m5out-paging-candidate --baseline m5out-paging-baseline.json
```

The comparison requires the same passing test set and exact simulated tick,
instruction/operation (where emitted), and TLB access/miss counts. Host speed
is excluded. The runner records the source revision, working-tree digest and
simulator/guest binary hashes when it starts; build the binary from that source
first. `source.tar.gz` retains `HEAD`, the binary-capable tracked patch and
untracked source files (including executable modes). To reconstruct the source,
check out the recorded HEAD in a separate checkout, apply `tracked.patch`, and
copy the archive's `untracked/` contents into that checkout. Preserve this
archive with `provenance.json`, configuration files, logs and statistics.
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

The shared transport and lifecycle extensions run separately so that adding
fixture cases does not change the original 56-case measurement set:

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

The native VMX operand guest runs independently of VM entry/exit and EPT:

```
python3 tests/gem5/x86_paging/run.py --vmx-operands-only --outdir m5out-vmx-operands
```

It covers VMXON, VMCLEAR, VMPTRLD, VMPTRST, VMREAD and VMWRITE with actual
register/memory operands on AtomicSimple, TimingSimple and O3, plus timing/O3
Ruby MESI Two Level. Checks include indexed/RIP-relative addressing, high
64-bit field values, split-page success/faults, unchanged flags on operand
faults, unsupported/read-only fields, and precheck-versus-operand fault
priority. This is operand execution coverage; full timing/O3 VM entry/exit and
event delivery remain separate EPT integration gates.

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
130–134 functional nonidentity read/write with no A/D changes; 140–143 INVLPG
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
