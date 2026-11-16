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
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py init
build/X86/gem5.opt tests/gem5/x86_interrupts/config.py thread-reset --cpu o3
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

`thread-reset` tests `ISA::resetThread()` directly, using a deterministic
power-on register baseline. It seeds dirty long-mode state, including all
integer, FP, and condition-code registers, then checks reset registers, the
reset-vector PC, real-mode decoding, and a TSC reset at a nonzero cycle. It
also checks repeated resets and preservation of thread status and APIC state.
The two-CPU Atomic case checks context isolation and the BSP-bit convention.

Hardware-reset FP values (FCW `0x0040`, FTW `0x5555`, MXCSR `0x1f80`) apply
only to the explicit thread-reset hook. Existing startup defaults and INIT
preservation remain unchanged. The hook starts from gem5's miscellaneous
register defaults, overrides MTRR enable/type to zero, and derives EDX from
the configured CPUID signature. Model-specific warm-reset exceptions are not
modeled.

This is a register-state hook: the caller owns decoder-buffer reset, MMU
flushing, execution status, and interrupt-controller reset. The hook updates
the decoder's execution mode, but does not reset its buffered bytes. BSP
election/AP startup sequencing, live-pipeline reset, cache/BTB invalidation,
KVM synchronization, and firmware boot are outside this fixture's scope.

The architectural reference is [Intel SDM Volume 3A][sdm], order 253668-085US,
October 2024, section 11.1 and Table 11-1. Interrupt clearing is a simulator
operation, not an architectural RESET, INIT, or APIC software-disable event.

[sdm]: https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf
