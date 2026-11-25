# Studying nested page walks in gem5

I use this fork to measure what Intel Extended Page Tables (EPT) add to
address translation. A small 64-bit guest runs under VMX in gem5. EPT
translates both the guest's page-table accesses and its final instruction
fetches, loads and stores. The guest tests exercise VM entry, EPT violations
and resuming the faulting instruction on AtomicSimple, TimingSimple, Minor
and O3.

The supported guest setup uses four-level EPT tables in write-back memory,
4 KiB EPT pages and optional accessed/dirty updates. The disk-free benchmark
runs the same shuffled chain of dependent loads with native paging, EPT, and
EPT with a combined translation cache. It checks the returned data and
reports simulated ticks and guest and EPT descriptor reads separately.

The combined cache defaults to zero entries. Enabling it adds entries
alongside the guest TLB. A usable hit skips both walks and adds zero lookup
cycles by default. The timing model still needs calibration against hardware.

To build and run the study from the repository root:

```sh
scons build/X86/gem5.opt -j4
python3 tests/gem5/x86_paging/run.py --combined-only --outdir m5out-combined
```

See the [paging test guide](tests/gem5/x86_paging/README.md) for the
workloads, counters and regression commands.
