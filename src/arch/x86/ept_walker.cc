/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/ept_walker.hh"

#include <memory>

#include "arch/x86/insts/vmx.hh"
#include "arch/x86/paging.hh"
#include "arch/x86/paging_port.hh"
#include "base/logging.hh"
#include "mem/packet_access.hh"

namespace gem5::X86ISA::ept
{
namespace
{

// One lookup's decoding state, independent of how a memory response arrives.
// There is no admission queue: the existing PagingPort owns transport policy.
struct WalkState
{
    const LookupInput input;
    const unsigned physicalBits;
    const uint64_t capability;
    const bool functional;
    unsigned level;
    Addr address = 0;
    uint8_t permissions = 7;
    // Empty while walking; initialization starts with a fail-closed result.
    std::optional<WalkResult> result = WalkResult{};
    struct Update
    {
        Addr address;
        unsigned level;
        uint64_t observed;
        uint64_t bits;
    };
    std::optional<Update> update;

    WalkState(const LookupInput &lookup, unsigned hostPhysicalBits,
              uint64_t caps, bool functional = false)
        : input(lookup), physicalBits(hostPhysicalBits), capability(caps),
          functional(functional),
          level(input.config.walkLength)
    {
        const auto &config = input.config;
        const auto decoded = decodeEptp(config.eptp, capability, physicalBits);
        if (!decoded || decoded->root != config.root ||
            decoded->walkLength != config.walkLength ||
            decoded->memoryType != config.memoryType ||
            decoded->accessedDirty != config.accessedDirty ||
            (input.access != EptAccess::Read &&
             input.access != EptAccess::Write &&
             input.access != EptAccess::Execute)) {
            return;
        }
        // A valid 52-bit GPA outside four-level EPT's range is an
        // architectural violation, not an invalid walker invocation.
        // No entry is consulted, so the reported EPT permissions are zero.
        if (config.walkLength == 4 && !(input.guestPhysical & ~mask(52)) &&
            bits(input.guestPhysical, 51, 48)) {
            result = WalkResult{WalkStatus::Violation};
            return;
        }
        const auto root = rootEntryAddress(input, physicalBits);
        if (!root)
            return;
        address = *root;
        result.reset();
    }

    bool done() const { return result.has_value() && !update; }

    void cancel()
    {
        update.reset();
        result = WalkResult{WalkStatus::Cancelled};
    }

    std::unique_ptr<Packet>
    nextPacket(RequestorID requestor) const
    {
        Request::Flags flags = Request::PHYSICAL;
        flags.set(Request::UNCACHEABLE, input.config.memoryType == 0);
        if (update)
            flags.set(Request::ATOMIC_RETURN_OP);
        auto request = std::make_shared<Request>(
            update ? update->address : address, sizeof(uint64_t), flags,
            requestor);
        if (update) {
            request->setAtomicOpFunctor(
                std::make_unique<paging::ConditionalUpdate<uint64_t>>(
                    update->observed, update->bits));
            request->setExtraData(update->observed);
        }
        auto packet = std::make_unique<Packet>(request,
            update ? MemCmd::SwapReq : MemCmd::ReadReq);
        packet->allocate();
        return packet;
    }

    void
    consume(const Packet &packet)
    {
        WalkResult terminal;
        terminal.level = update ? update->level : level;
        terminal.entryAddress = update ? update->address : address;
        if (packet.isError()) {
            terminal.status = WalkStatus::MemoryError;
            update.reset();
            result = terminal;
            return;
        }

        if (update) {
            const bool replaced = packet.getLE<uint64_t>() != update->observed;
            update.reset();
            if (replaced) {
                // Never use a child pointer, rights or leaf from the old
                // observation. Already-completed A updates remain sticky.
                level = input.config.walkLength;
                address = *rootEntryAddress(input, physicalBits);
                permissions = 7;
                result.reset();
            }
            return;
        }

        terminal.entry = packet.getLE<uint64_t>();
        const EptAccess access = input.config.accessedDirty &&
            input.pagingStructure ? EptAccess::Write : input.access;
        const auto decoded = decodeEntry(terminal.entry, level, physicalBits,
                                         capability, permissions, access);
        panic_if(!decoded, "EPT walk supplied invalid decoder inputs");
        if (input.config.accessedDirty && !functional &&
            (terminal.entry & 7) &&
            decoded->kind != EntryKind::Misconfiguration) {
            // A records each valid entry used, including walks that later
            // fault. D is set only for an allowed write to a leaf mapping.
            const uint64_t wanted = (1ULL << 8) |
                (decoded->kind == EntryKind::Mapping &&
                 access == EptAccess::Write ? 1ULL << 9 : 0);
            if (const auto missing = wanted & ~terminal.entry; missing)
                update = Update{address, level, terminal.entry, missing};
        }
        switch (decoded->kind) {
          case EntryKind::NextTable: {
            const auto next = nextEntryAddress(input, *decoded, level,
                                               physicalBits);
            panic_if(!next, "Decoded EPT table has no child entry address");
            permissions = decoded->permissions;
            address = *next;
            --level;
            return;
          }
          case EntryKind::Mapping:
            terminal.status = WalkStatus::Mapping;
            terminal.hostPhysical = decoded->base |
                (input.guestPhysical & mask(decoded->pageShift));
            terminal.permissions = decoded->permissions;
            terminal.pageShift = decoded->pageShift;
            terminal.memoryType = decoded->memoryType;
            terminal.ignorePat = bits(terminal.entry, 6);
            break;
          case EntryKind::Violation:
            terminal.status = WalkStatus::Violation;
            terminal.permissions = decoded->permissions;
            break;
          case EntryKind::Misconfiguration:
            terminal.status = WalkStatus::Misconfiguration;
            break;
        }
        result = terminal;
    }
};

struct TimingWalk : std::enable_shared_from_this<TimingWalk>
{
    WalkState state;
    PagingPort &port;
    const RequestorID requestor;
    const std::function<bool()> valid;
    std::function<void(const WalkResult &)> completion;

    TimingWalk(PagingPort &port, RequestorID requestor,
               const LookupInput &input, unsigned physicalBits,
               uint64_t capability, std::function<bool()> valid,
               std::function<void(const WalkResult &)> completion)
        : state(input, physicalBits, capability), port(port),
          requestor(requestor), valid(std::move(valid)),
          completion(std::move(completion))
    {}

    void
    advance()
    {
        // Obsolete results must not escape, including errors and preflight
        // failures. Cancellation is independent of architectural decoding.
        if (!valid())
            state.cancel();
        if (state.done()) {
            auto done = std::move(completion);
            done(*state.result);
            return;
        }
        port.submit(state.nextPacket(requestor).release(), valid,
            [self = shared_from_this()](PacketPtr packet, bool cancelled) {
                self->receive(packet, cancelled);
            });
    }

    void
    receive(PacketPtr response, bool cancelled)
    {
        // A queued cancellation returns a request, not a data response. An
        // already-issued request cannot be withdrawn: retain it until here.
        std::unique_ptr<Packet> packet(response);
        if (cancelled || !valid())
            state.cancel();
        else
            state.consume(*packet);
        packet.reset();
        advance();
    }
};

} // anonymous namespace

Fault
lookupFault(const WalkResult &result, const LookupInput &input,
            Addr guestLinear, bool finalAccess)
{
    if (result.status == WalkStatus::Mapping)
        return NoFault;
    if (result.status == WalkStatus::Cancelled)
        return std::make_shared<ReExec>();
    panic_if(result.status != WalkStatus::Violation &&
             result.status != WalkStatus::Misconfiguration,
             "EPT lookup failed internally: %d", int(result.status));

    VmxExitInfo info;
    info.reason = result.status == WalkStatus::Violation ?
        VmxExitReason::EptViolation : VmxExitReason::EptMisconfiguration;
    info.hasGuestPhysicalAddress = true;
    info.guestPhysicalAddress = input.guestPhysical;
    if (result.status == WalkStatus::Violation) {
        info.hasQualification = true;
        info.qualification = input.config.accessedDirty && input.pagingStructure ?
            3 : 1ULL << unsigned(input.access);
        info.qualification |= uint64_t(result.permissions) << 3;
        info.qualification |= 1ULL << 7; // Guest linear address is valid.
        if (finalAccess)
            info.qualification |= 1ULL << 8;
        info.hasGuestLinearAddress = true;
        info.guestLinearAddress = guestLinear;
    }
    return std::make_shared<VmxExitFault>(info);
}

AtomicResult
walkAtomic(PagingPort &port, RequestorID requestor, const LookupInput &input,
           unsigned hostPhysicalBits, uint64_t capability)
{
    WalkState state(input, hostPhysicalBits, capability);
    Tick latency = 0;
    while (!state.done()) {
        auto packet = state.nextPacket(requestor);
        latency += port.sendAtomic(packet.get());
        state.consume(*packet);
    }
    return {*state.result, latency};
}

WalkResult
walkFunctional(PagingPort &port, RequestorID requestor,
               const LookupInput &input, unsigned hostPhysicalBits,
               uint64_t capability)
{
    WalkState state(input, hostPhysicalBits, capability, true);
    while (!state.done()) {
        auto packet = state.nextPacket(requestor);
        port.sendFunctional(packet.get());
        state.consume(*packet);
    }
    return *state.result;
}

void
walkTiming(PagingPort &port, RequestorID requestor, const LookupInput &input,
           unsigned hostPhysicalBits, uint64_t capability,
           std::function<bool()> valid,
           std::function<void(const WalkResult &)> completion)
{
    panic_if(!valid || !completion, "EPT timing lookup requires callbacks");
    auto walk = std::make_shared<TimingWalk>(port, requestor, input,
        hostPhysicalBits, capability, std::move(valid), std::move(completion));
    walk->advance();
}

} // namespace gem5::X86ISA::ept
