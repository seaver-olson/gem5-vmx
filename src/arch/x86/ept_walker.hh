/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __ARCH_X86_EPT_WALKER_HH__
#define __ARCH_X86_EPT_WALKER_HH__

#include <functional>

#include "arch/x86/ept.hh"
#include "mem/request.hh"
#include "sim/faults.hh"

namespace gem5::X86ISA
{
class PagingPort;

namespace ept
{

// These are lookup outcomes, not CPU faults or VM exits. In particular,
// invalid caller inputs and memory errors must not masquerade as EPT faults.
enum class WalkStatus : uint8_t
{
    Mapping, Violation, Misconfiguration, InvalidInput, MemoryError, Cancelled
};

struct WalkResult
{
    WalkStatus status = WalkStatus::InvalidInput;

    // Translation fields are valid only for Mapping, except permissions,
    // which also records effective R/W/X for a Violation's qualification.
    // memoryType/ignorePat describe the EPT leaf, not the final memory type.
    Addr hostPhysical = 0;
    uint8_t permissions = 0;
    uint8_t pageShift = 0;
    uint8_t memoryType = 0;
    bool ignorePat = false;

    // Terminal-entry diagnostics for Mapping/Violation/Misconfiguration.
    // MemoryError has a level and address, but no valid raw entry value.
    // Preflight failures leave all three fields zero.
    unsigned level = 0;
    Addr entryAddress = 0;
    // Raw value decoded, before this walk's own A/D update.
    uint64_t entry = 0;
};

struct AtomicResult
{
    WalkResult result;
    Tick latency = 0;
};

// A failed second-stage lookup is never a guest page fault. The GPA is the
// address that EPT tried to translate; finalAccess distinguishes data/fetch
// from a guest paging-structure access in the VM-exit qualification.
Fault lookupFault(const WalkResult &result, const LookupInput &input,
                  Addr guestLinear, bool finalAccess);

// Lookup through the caller's physical paging port. All arguments
// are captured, including the capability MSR and host address width. No live
// VMCS/CPU state, TLB fill or HPA data access is performed. With EPT A/D
// enabled, conditional physical updates complete before advancing the walk;
// a replaced descriptor restarts from the captured root. EPTP UC/WB controls
// table-read/update caching; this standalone API assumes CR0.CD is clear.
// The caller must use an atomic-capable memory hierarchy. Latency is the sum
// of sendAtomic return values; this function does not advance curTick().
AtomicResult walkAtomic(PagingPort &port, RequestorID requestor,
                        const LookupInput &input, unsigned hostPhysicalBits,
                        uint64_t capability);

// Same lookup and validation, using read-only functional memory accesses.
// Works in atomic and timing hierarchies without advancing simulated time,
// issuing timing requests, filling a TLB, or modifying EPT entries.
WalkResult walkFunctional(PagingPort &port, RequestorID requestor,
                          const LookupInput &input, unsigned hostPhysicalBits,
                          uint64_t capability);

// Asynchronous lookup with at most one outstanding entry read or update.
// PagingPort owns retries and keeps callback-owned state alive until response or
// queued-request cancellation. An issued update may modify memory even when
// its result is cancelled. The borrowed port must outlive that work.
// Both callbacks are required. valid() must remain false once obsolete; it
// is checked before issue/retry and again before using a response/result.
// Cancellation returns Cancelled, never an EPT fault. Already-obsolete and
// preflight-failing lookups may complete inline; all complete exactly once.
// The result reference is valid only during completion (copy to retain it).
void walkTiming(PagingPort &port, RequestorID requestor,
                const LookupInput &input, unsigned hostPhysicalBits,
                uint64_t capability, std::function<bool()> valid,
                std::function<void(const WalkResult &)> completion);

} // namespace ept
} // namespace gem5::X86ISA

#endif // __ARCH_X86_EPT_WALKER_HH__
