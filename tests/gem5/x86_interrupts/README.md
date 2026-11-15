# x86 interrupt operation semantics

This fixture uses a halted AtomicSimpleCPU's real ThreadContext and a
test-only local APIC subclass. It does not boot a guest or execute CPU
instructions. TestLib enables `USE_TEST_OBJECTS` when building the simulator.

```sh
scons defconfig build/X86 build_opts/X86
scons setconfig build/X86 USE_TEST_OBJECTS=y
scons build/X86/gem5.opt -j4
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py clear
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py init
```

`clear` verifies that pending interrupts disappear while APIC configuration,
in-service state, startup status, outgoing IPI accounting, and the armed timer
remain intact. Advancing simulated time proves that the timer can deliver a
new request afterward. It also checks the generic controller reset fallback.

`init` checks the initial FP defaults, then verifies that INIT preserves
nondefault x87/SIMD register and control state while retaining core
initialization and the wait-for-SIPI continuation.

The `reset` scenario must terminate unsuccessfully with
`Local APIC RESET is not implemented.` It checks the distinct controller hook;
CPU RESET remains unsupported on x86 and is not exercised by this fixture.

The architectural reference is [Intel SDM Volume 3A][sdm], order 253668-085US,
October 2024, section 11.1 and Table 11-1. Interrupt clearing is a simulator
operation, not an architectural RESET, INIT, or APIC software-disable event.

[sdm]: https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf
