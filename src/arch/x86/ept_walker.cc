/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/ept_walker.hh"

#include <memory>

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
    unsigned level;
    Addr address = 0;
    uint8_t permissions = 7;
    // Empty while walking; initialization starts with a fail-closed result.
    std::optional<WalkResult> result = WalkResult{};

    WalkState(const LookupInput &lookup, unsigned hostPhysicalBits,
              uint64_t caps)
        : input(lookup), physicalBits(hostPhysicalBits), capability(caps),
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
        const auto root = rootEntryAddress(input, physicalBits);
        if (!root)
            return;
        if (config.accessedDirty) {
            result->status = WalkStatus::UnsupportedConfig;
            return;
        }
        address = *root;
        result.reset();
    }

    std::unique_ptr<Packet>
    readPacket(RequestorID requestor) const
    {
        Request::Flags flags = Request::PHYSICAL;
        flags.set(Request::UNCACHEABLE, input.config.memoryType == 0);
        auto request = std::make_shared<Request>(address, sizeof(uint64_t),
                                                 flags, requestor);
        auto packet = std::make_unique<Packet>(request, MemCmd::ReadReq);
        packet->allocate();
        return packet;
    }

    void
    consume(const Packet &packet)
    {
        WalkResult terminal;
        terminal.level = level;
        terminal.entryAddress = address;
        if (packet.isError()) {
            terminal.status = WalkStatus::MemoryError;
            result = terminal;
            return;
        }

        terminal.entry = packet.getLE<uint64_t>();
        const auto decoded = decodeEntry(terminal.entry, level, physicalBits,
                                         capability, permissions,
                                         input.access);
        panic_if(!decoded, "EPT walk supplied invalid decoder inputs");
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
            state.result = WalkResult{WalkStatus::Cancelled};
        if (state.result) {
            auto done = std::move(completion);
            done(*state.result);
            return;
        }
        port.submit(state.readPacket(requestor).release(), valid,
            [self = shared_from_this()](PacketPtr packet, bool cancelled) {
                self->receive(packet, cancelled);
            });
    }

    void
    receive(PacketPtr response, bool cancelled)
    {
        // A queued cancellation returns a request, not a data response. An
        // already-issued read cannot be withdrawn: retain it until here.
        std::unique_ptr<Packet> packet(response);
        if (cancelled || !valid())
            state.result = WalkResult{WalkStatus::Cancelled};
        else
            state.consume(*packet);
        packet.reset();
        advance();
    }
};

} // anonymous namespace

AtomicResult
walkAtomic(PagingPort &port, RequestorID requestor, const LookupInput &input,
           unsigned hostPhysicalBits, uint64_t capability)
{
    WalkState state(input, hostPhysicalBits, capability);
    Tick latency = 0;
    while (!state.result) {
        auto packet = state.readPacket(requestor);
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
    WalkState state(input, hostPhysicalBits, capability);
    while (!state.result) {
        auto packet = state.readPacket(requestor);
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
