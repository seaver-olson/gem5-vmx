# x86 interrupt operation semantics

This fixture uses real ThreadContexts and a test-only local APIC subclass.
Interrupt tests use a halted AtomicSimpleCPU; thread-reset tests use suspended
AtomicSimpleCPU, TimingSimpleCPU, MinorCPU, and O3 contexts. It does not boot a
guest or execute CPU instructions. TestLib enables `USE_TEST_OBJECTS` when
building the simulator.

```sh
scons defconfig build/X86 build_opts/X86
scons setconfig build/X86 USE_TEST_OBJECTS=y
scons build/X86/gem5.opt -j4
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py clear
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py eoi
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py init
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py thread-reset --cpu o3
```

To discover and run all nine quick suites, including the expected RESET
failure, after building:

```sh
python3 tests/main.py list tests/gem5/x86_interrupts \
    --isa=X86 --variant=opt -q --suites
python3 tests/main.py run tests/gem5/x86_interrupts \
    --isa=X86 --variant=opt --skip-build -v
```

There are 18 TestLib checks: execution and an output verifier for each suite.
The same suites are registered for `--isa=ALL`. C++ check failures identify
the scenario, context, reset attempt (zero outside the reset loop), field or
register index, expected value, and actual value.

`clear` verifies that pending interrupts disappear while APIC configuration,
in-service state, startup status, synthetic outgoing IPI accounting, and the
armed timer remain intact. The IPI check only seeds and compares `pendingIPIs`
and the ICR delivery-status bit; it sends no IPI and does not exercise
`completeIPI()` or response delivery. Advancing simulated time proves that
the timer can deliver a new request afterward. The scenario also checks the
generic controller reset fallback.

`eoi` accepts a level-triggered vector into service, queues that vector again
alongside a pending-only level vector, and calls `clearAll()`. It checks that
both requests disappear, the in-service vector retains its TMR bit, and the
pending-only vector loses its TMR bit. An EOI must retire the ISR entry
without making a discarded request deliverable. A fresh edge-triggered
request for the same vector must then replace the old trigger metadata and
remain deliverable. This tests gem5's local ISR/IRR/TMR bookkeeping; it does
not validate IOAPIC EOI broadcasts or remote-IRR behavior.

`init` checks the initial FP defaults, then verifies that INIT preserves
nondefault x87/SIMD register and control state while retaining core
initialization and the wait-for-SIPI continuation.

The automated `reset` case requires both `SIGABRT` and the exact panic text
`Local APIC RESET is not implemented.` A successful return, another exit
status, or an unrelated panic fails the suite. Core dumps are disabled for
this intentional abort. It checks the distinct controller hook directly;
the full `BaseCPU::setReset()` path remains unsupported on x86 and is not
exercised by this fixture.

`thread-reset` tests `ISA::resetThread()` directly, using a deterministic
power-on register baseline. It seeds dirty long-mode state, including all
integer, FP, and condition-code registers, then checks reset registers, the
reset-vector PC, real-mode decoding, and a TSC reset at a nonzero cycle. It
also checks repeated resets and preservation of thread status and APIC state.
The two-CPU Atomic case checks that resetting the first context leaves the
second context's seeded RAX, CR0, and first FP register unchanged. Each
context also receives its own CPUID signature and the expected BSP bit.

Hardware-reset FP values (FCW `0x0040`, FTW `0x5555`, MXCSR `0x1f80`) apply
only to the explicit thread-reset hook. Existing startup defaults and INIT
preservation remain unchanged. The hook starts from gem5's miscellaneous
register defaults, overrides MTRR enable/type to zero, and derives EDX from
the configured CPUID signature. Model-specific warm-reset exceptions are not
modeled.

These checks distinguish architectural reset/INIT values from gem5 choices:
startup FTW `0xffff`, capability-register defaults, scratch-register clearing,
the PC/microPC representation, and context-zero BSP assignment are simulator
conventions. They do not establish model-specific hardware reset fidelity.

This is a register-state hook: the caller owns decoder-buffer reset, MMU
flushing, execution status, and interrupt-controller reset. The hook updates
the decoder's execution mode, but does not reset its buffered bytes. BSP
election/AP startup sequencing, live-pipeline reset, cache/BTB invalidation,
KVM synchronization, and firmware boot are outside this fixture's scope.

The architectural reference is [Intel SDM Volume 3A][sdm], order 253668-085US,
October 2024, section 11.1 and Table 11-1. Interrupt clearing is a simulator
operation, not an architectural RESET, INIT, or APIC software-disable event.

[sdm]: https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf
