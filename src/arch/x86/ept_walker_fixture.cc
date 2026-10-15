/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/paging_tester.hh"

#include <array>

#include "arch/x86/ept_walker.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/tlb.hh"
#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "mem/port_proxy.hh"
#include "sim/system.hh"

namespace gem5::X86ISA
{
namespace
{

void
checkResult(const ept::WalkResult &actual, const ept::WalkResult &expected,
            const char *label)
{
    panic_if(actual.status != expected.status ||
             actual.hostPhysical != expected.hostPhysical ||
             actual.permissions != expected.permissions ||
             actual.pageShift != expected.pageShift ||
             actual.memoryType != expected.memoryType ||
             actual.ignorePat != expected.ignorePat ||
             actual.level != expected.level ||
             actual.entryAddress != expected.entryAddress ||
             actual.entry != expected.entry,
             "EPT %s: lookup modes returned different results", label);
}

} // anonymous namespace

void
PagingTester::eptCases()
{
    using ept::WalkStatus;
    // Test capabilities only: this does not change the ISA's advertised MSR.
    constexpr uint64_t Caps = 1ULL | (1ULL << 6) | (1ULL << 7) |
        (1ULL << 8) | (1ULL << 14) | (1ULL << 16) | (1ULL << 17) |
        (1ULL << 21);
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    EptConfig config{};
    Addr gpa = 0;
    Addr mappingBase = 0;
    unsigned physicalBits = 48;
    unsigned leafLevel = 1;
    unsigned errorRead = 0;
    uint64_t capability = Caps;
    unsigned checks = 0;

    const auto setup = [&](unsigned levels = 4, unsigned leaf = 1,
                           uint8_t memoryType = 6) {
        std::array<uint8_t, 5 * 4096> zero{};
        system->physProxy.writeBlob(Tables, zero.data(), zero.size());
        descriptors.clear();
        descriptorSize = 8;
        physicalBits = 48;
        capability = Caps;
        errorRead = 0;
        leafLevel = leaf;
        // The translated HPA is deliberately outside installed RAM. Lookup
        // must return it, not read it or silently substitute the GPA.
        mappingBase = 0x80000000;
        gpa = 0xabc;
        for (unsigned level = 1; level <= levels; ++level)
            gpa |= Addr(3 * level + 1) << (12 + 9 * (level - 1));
        const uint64_t eptp = Tables | ((levels - 1) << 3) | memoryType;
        config = *ept::decodeEptp(eptp, capability, physicalBits);
        for (unsigned level = levels; level >= leaf; --level) {
            const Addr table = Tables + (levels - level) * 4096;
            const Addr slot = (gpa >> (12 + 9 * (level - 1))) & 511;
            descriptors.push_back(table + 8 * slot);
            const uint64_t raw = level == leaf ?
                mappingBase | 7 | (6 << 3) | (leaf > 1 ? 128 : 0) :
                (table + 4096) | 7;
            descriptor(descriptors.size() - 1, raw);
        }
    };

    const auto checkMode = [&](bool functional, const char *label,
                               WalkStatus expected, unsigned reads,
                               EptAccess access, uint8_t permissions) {
        std::vector<uint64_t> before;
        for (unsigned i = 0; i < descriptors.size(); ++i)
            before.push_back(descriptor(i));
        unsigned seen = 0;
        observedAtomicLatency = 0;
        inspectAttributes = [&](PacketPtr packet) {
            panic_if(seen >= reads || packet->getAddr() != descriptors[seen],
                     "EPT %s: unexpected entry read %u at %#x", label, seen,
                     packet->getAddr());
            panic_if(packet->cmd != MemCmd::ReadReq ||
                     packet->getSize() != 8 ||
                     !packet->req->getFlags().isSet(Request::PHYSICAL) ||
                     packet->req->requestorId() != requestor ||
                     packet->req->isUncacheable() != (config.memoryType == 0),
                     "EPT %s: incorrect physical read request", label);
            const auto attributes = PagingPort::attributes(packet);
            panic_if(attributes.pwt || attributes.pcd,
                     "EPT %s: guest paging attributes leaked into EPT", label);
            if (++seen == errorRead) {
                packet->makeResponse();
                packet->setBadAddress();
            }
        };
        const Tick start = curTick();
        const ept::LookupInput input(gpa, access, config);
        ept::WalkResult result;
        Tick latency = 0;
        if (functional) {
            result = ept::walkFunctional(port, requestor, input, physicalBits,
                                         capability);
        } else {
            const auto lookup = ept::walkAtomic(port, requestor, input,
                                                physicalBits, capability);
            result = lookup.result;
            latency = lookup.latency;
        }
        inspectAttributes = {};
        panic_if(result.status != expected || seen != reads,
                 "EPT %s: status %u/%u, reads %u/%u", label,
                 unsigned(result.status), unsigned(expected), seen, reads);
        panic_if(latency != observedAtomicLatency ||
                 curTick() != start ||
                 (!functional && reads > 0 && errorRead != 1 &&
                  latency == 0) ||
                 (reads == 0 && latency != 0),
                 "EPT %s: incorrect %s latency", label,
                 functional ? "functional" : "atomic");
        panic_if(!port.idle(), "EPT %s: retained port ownership", label);
        if (reads) {
            panic_if(result.level != config.walkLength - reads + 1 ||
                     result.entryAddress != descriptors[reads - 1] ||
                     (expected != WalkStatus::MemoryError &&
                      result.entry != before[reads - 1]),
                     "EPT %s: incorrect terminal diagnostics", label);
        } else {
            panic_if(result.level || result.entryAddress || result.entry,
                     "EPT %s: preflight failure reported an entry", label);
        }
        if (expected == WalkStatus::Mapping) {
            const unsigned shift = 12 + 9 * (leafLevel - 1);
            const Addr expectedHpa = mappingBase | (gpa & mask(shift));
            panic_if(result.hostPhysical != expectedHpa ||
                     result.permissions != permissions ||
                     result.pageShift != shift ||
                     result.memoryType != bits(before.back(), 5, 3) ||
                     result.ignorePat != bool(bits(before.back(), 6)),
                     "EPT %s: incorrect mapping or leaf attributes", label);
        } else {
            panic_if(result.hostPhysical || result.permissions ||
                     result.pageShift || result.memoryType || result.ignorePat,
                     "EPT %s: failure exposed a usable mapping", label);
        }
        for (unsigned i = 0; i < before.size(); ++i)
            panic_if(descriptor(i) != before[i],
                     "EPT %s: walk modified a table entry", label);
        ++checks;
        return result;
    };
    const auto check = [&](const char *label, WalkStatus expected,
                           unsigned reads, EptAccess access = EptAccess::Read,
                           uint8_t permissions = 7) {
        // Functional runs first, so it also sees cold tables, not just lines
        // already brought into the cache by an atomic lookup.
        const auto functional = checkMode(true, label, expected, reads, access,
                                           permissions);
        if (system->isAtomicMode()) {
            const auto atomic = checkMode(false, label, expected, reads,
                                           access, permissions);
            checkResult(atomic, functional, label);
        } else {
            // Replay the same tables asynchronously after the synchronous
            // suite finishes. Capture values, not this function's locals.
            const ept::LookupInput input(gpa, access, config);
            const auto slots = descriptors;
            std::vector<uint64_t> entries;
            for (unsigned i = 0; i < slots.size(); ++i)
                entries.push_back(descriptor(i));
            cases.emplace_back([=, this] {
                descriptors = slots;
                descriptorSize = 8;
                for (unsigned i = 0; i < entries.size(); ++i)
                    descriptor(i, entries[i]);
                struct Progress
                {
                    unsigned responses = 0;
                    unsigned callbacks = 0;
                    Tick lastResponse = 0;
                };
                auto progress = std::make_shared<Progress>();
                inspectAttributes = [=, this](PacketPtr packet) {
                    const unsigned next = progress->responses;
                    panic_if(pendingPackets || next >= reads ||
                             packet->getAddr() != slots[next] ||
                             packet->getSize() != 8 ||
                             packet->cmd != MemCmd::ReadReq ||
                             packet->req->requestorId() != requestor ||
                             !packet->req->getFlags().isSet(
                                 Request::PHYSICAL) ||
                             packet->req->isUncacheable() !=
                                 (input.config.memoryType == 0),
                             "EPT %s: invalid timing request", label);
                    const auto attrs = PagingPort::attributes(packet);
                    panic_if(attrs.pwt || attrs.pcd,
                             "EPT %s: leaked guest attributes", label);
                };
                inspectResponse = [=](PacketPtr packet) {
                    panic_if(progress->responses >= reads ||
                             packet->getAddr() != slots[progress->responses],
                             "EPT %s: response out of order", label);
                    if (++progress->responses == errorRead)
                        packet->setBadAddress();
                    progress->lastResponse = curTick();
                };
                const Tick start = curTick();
                awaiting = true;
                ept::walkTiming(tlb->getWalker()->pagingPort(), requestor,
                    input, physicalBits, capability, [] { return true; },
                    [=, this](const ept::WalkResult &result) {
                        panic_if(++progress->callbacks != 1 || !awaiting ||
                                 progress->responses != reads,
                                 "EPT %s: incorrect timing completion", label);
                        checkResult(result, functional, label);
                        auto &port = tlb->getWalker()->pagingPort();
                        panic_if(port.outstanding() != (reads ? 1 : 0) ||
                                 (reads ? curTick() <= start ||
                                     curTick() != progress->lastResponse :
                                     curTick() != start),
                                 "EPT %s: wrong timing/ownership", label);
                        for (unsigned i = 0; i < entries.size(); ++i)
                            panic_if(descriptor(i) != entries[i],
                                     "EPT %s: timing modified tables", label);
                        inspectAttributes = {};
                        inspectResponse = {};
                        awaiting = false;
                        schedule(nextEvent, curTick() + 1);
                    });
                panic_if(reads && (!awaiting || progress->callbacks),
                         "EPT %s: memory lookup completed inline", label);
            });
        }
    };

    for (unsigned levels : {4, 5}) {
        for (unsigned leaf : {1, 2, 3}) {
            for (uint8_t type : {0, 6}) {
                setup(levels, leaf, type);
                for (auto access : {EptAccess::Read, EptAccess::Write,
                                     EptAccess::Execute}) {
                    check("mapping", WalkStatus::Mapping, levels - leaf + 1,
                          access);
                }
            }
        }
    }
    for (unsigned width : {32, 52}) {
        for (unsigned leaf : {1, 2, 3}) {
            setup(4, leaf);
            physicalBits = width;
            const unsigned shift = 12 + 9 * (leaf - 1);
            mappingBase = (1ULL << width) - (1ULL << shift);
            descriptor(descriptors.size() - 1,
                       mappingBase | 7 | (6 << 3) | (leaf > 1 ? 128 : 0));
            gpa &= ~mask(shift);
            check("first page byte", WalkStatus::Mapping, 5 - leaf);
            gpa |= mask(shift);
            check("last host-physical byte", WalkStatus::Mapping, 5 - leaf);
        }
    }
    setup();
    descriptor(0, descriptor(0) & ~2ULL); // ancestor removes W
    descriptor(1, descriptor(1) & ~4ULL); // another removes X
    check("intersected read", WalkStatus::Mapping, 4, EptAccess::Read, 1);
    check("denied write", WalkStatus::Violation, 4, EptAccess::Write);
    check("denied execute", WalkStatus::Violation, 4, EptAccess::Execute);
    descriptor(3, (descriptor(3) & ~7ULL) | 4); // X-only leaf
    check("empty intersection", WalkStatus::Violation, 4, EptAccess::Execute);
    for (unsigned i = 0; i < 4; ++i)
        descriptor(i, (descriptor(i) & ~7ULL) | 4);
    check("execute-only", WalkStatus::Mapping, 4, EptAccess::Execute, 4);
    check("denied read", WalkStatus::Violation, 4, EptAccess::Read);
    capability &= ~1ULL;
    check("unsupported execute-only", WalkStatus::Misconfiguration, 1);

    for (unsigned levels : {4, 5}) {
        for (unsigned stop = 0; stop < levels; ++stop) {
            setup(levels);
            // Absent entries win over otherwise-reserved contents.
            descriptor(stop, (1ULL << 48) | 128 | (7 << 3));
            check("absent", WalkStatus::Violation, stop + 1);
            descriptor(stop, (descriptor(stop) & ~7ULL) | 2);
            check("W without R", WalkStatus::Misconfiguration, stop + 1);
        }
    }
    setup();
    descriptor(0, descriptor(0) & ~2ULL);
    descriptor(3, (descriptor(3) & ~7ULL) | 2);
    check("misconfiguration below denial", WalkStatus::Misconfiguration, 4,
          EptAccess::Write);
    setup();
    descriptor(1, descriptor(1) | 8);
    check("reserved non-leaf", WalkStatus::Misconfiguration, 2);
    setup();
    descriptor(2, descriptor(2) | (1ULL << 48));
    check("wide table address", WalkStatus::Misconfiguration, 3);
    for (unsigned leaf : {1, 2, 3}) {
        setup(4, leaf);
        descriptor(descriptors.size() - 1, descriptor(descriptors.size() - 1) |
                   (1ULL << 48));
        check("wide leaf address", WalkStatus::Misconfiguration, 5 - leaf);
    }
    for (unsigned leaf : {2, 3}) {
        setup(4, leaf);
        capability &= ~(1ULL << (leaf == 2 ? 16 : 17));
        check("unsupported leaf", WalkStatus::Misconfiguration, 5 - leaf);
        capability = Caps;
        descriptor(descriptors.size() - 1, descriptor(descriptors.size() - 1) |
                   4096);
        check("misaligned large page", WalkStatus::Misconfiguration, 5 - leaf);
    }
    for (uint8_t type : {0, 1, 2, 3, 4, 5, 6, 7}) {
        setup();
        descriptor(3, (descriptor(3) & ~(7ULL << 3)) | (type << 3) | 64);
        check("leaf type and IPAT", type == 2 || type == 3 || type == 7 ?
              WalkStatus::Misconfiguration : WalkStatus::Mapping, 4);
    }
    setup();
    check("before replacement", WalkStatus::Mapping, 4);
    mappingBase = 0x40000000;
    descriptor(3, mappingBase | 7 | (6 << 3));
    check("after replacement", WalkStatus::Mapping, 4);
    for (unsigned stop : {1, 3, 4}) {
        setup();
        errorRead = stop;
        check("memory error", WalkStatus::MemoryError, stop);
    }

    setup();
    check("invalid access", WalkStatus::InvalidInput, 0,
          static_cast<EptAccess>(99));
    for (unsigned width : {31, 53}) {
        physicalBits = width;
        check("invalid host width", WalkStatus::InvalidInput, 0);
    }
    for (unsigned levels : {4, 5}) {
        setup(levels);
        gpa |= 1ULL << (12 + 9 * levels);
        check("GPA outside walk width", WalkStatus::InvalidInput, 0);
    }
    for (unsigned field = 0; field < 5; ++field) {
        setup();
        switch (field) {
          case 0: config.root += 4096; break;
          case 1: config.walkLength = 5; break;
          case 2: config.memoryType = 0; break;
          case 3: config.accessedDirty = true; break;
          case 4: config.eptp |= 128; break;
        }
        check("inconsistent captured config", WalkStatus::InvalidInput, 0);
    }
    for (unsigned bit : {6, 14}) {
        setup();
        capability &= ~(1ULL << bit);
        check("unsupported EPTP capability", WalkStatus::InvalidInput, 0);
    }
    setup();
    config = *ept::decodeEptp(config.eptp | 64, capability, physicalBits);
    check("A/D not implemented", WalkStatus::UnsupportedConfig, 0);
    inform("EPT fixture: %u lookups passed (%s)", checks,
           system->isAtomicMode() ? "functional + atomic" : "functional only");
    if (system->isTimingMode()) {
        // Stop 0 cancels before admission. Other stops select the level's
        // request boundary, either rejected/queued or already issued.
        cases.emplace_back([this] { eptTimingLifecycle(false, true, 0); });
        for (unsigned stop : {1, 2, 4}) {
            cases.emplace_back([=, this] {
                eptTimingLifecycle(true, false, stop);
            });
            cases.emplace_back([=, this] {
                eptTimingLifecycle(true, true, stop);
            });
            cases.emplace_back([=, this] {
                eptTimingLifecycle(false, true, stop);
            });
        }
        cases.emplace_back([this] { eptTimingConcurrent(); });
    }
}

void
PagingTester::eptTimingLifecycle(bool retry, bool cancel, unsigned stop)
{
    setup(Long);
    constexpr uint64_t Caps = (1ULL << 6) | (1ULL << 14);
    auto config = *ept::decodeEptp(Tables | (3 << 3) | 6, Caps, 48);
    descriptor(3, descriptor(3) | (6 << 3));
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    const auto expected = ept::walkFunctional(port, requestor,
        ept::LookupInput(Virtual, EptAccess::Read, config), 48, Caps);
    std::vector<uint64_t> entries;
    for (unsigned i = 0; i < descriptors.size(); ++i)
        entries.push_back(descriptor(i));
    struct Progress
    {
        unsigned responses = 0;
        unsigned callbacks = 0;
        bool triggered = false;
    };
    auto progress = std::make_shared<Progress>();
    auto valid = std::make_shared<bool>(stop != 0);
    std::weak_ptr<bool> lifetime = valid;
    squash = [valid] { *valid = false; };
    inspectAttributes = [=, this](PacketPtr packet) {
        panic_if(pendingPackets || progress->responses >= 4 ||
                 packet->getAddr() != descriptors[progress->responses] ||
                 packet->cmd != MemCmd::ReadReq || packet->getSize() != 8,
                 "EPT lifecycle: unexpected read or overlapping requests");
        if (!progress->triggered && progress->responses + 1 == stop) {
            progress->triggered = true;
            injection = retry ? (cancel ? SquashRetryRead : RetryRead) :
                                SquashRead;
        }
    };
    inspectResponse = [=, this](PacketPtr packet) {
        panic_if(packet->getAddr() != descriptors[progress->responses],
                 "EPT lifecycle: response out of order");
        // Cancellation must discard even a failed response, not publish it.
        if (++progress->responses == stop && cancel && !retry)
            packet->setBadAddress();
    };
    const Tick start = curTick();
    awaiting = true;
    ept::walkTiming(port, requestor,
        ept::LookupInput(Virtual, EptAccess::Read, config), 48, Caps,
        [valid] { return *valid; },
        [=, this](const ept::WalkResult &result) {
            const unsigned reads = !cancel ? 4 :
                stop == 0 ? 0 : retry ? stop - 1 : stop;
            panic_if(++progress->callbacks != 1 || !awaiting ||
                     progress->responses != reads || pendingPackets ||
                     injected != (stop ? 1 : 0),
                     "EPT lifecycle: incorrect completion or read count");
            checkResult(result, cancel ?
                ept::WalkResult{ept::WalkStatus::Cancelled} : expected,
                "lifecycle");
            auto &port = tlb->getWalker()->pagingPort();
            panic_if(port.outstanding() != (stop ? 1 : 0) ||
                     (stop ? curTick() <= start : curTick() != start),
                     "EPT lifecycle: premature completion or retirement");
            for (unsigned i = 0; i < entries.size(); ++i)
                panic_if(descriptor(i) != entries[i],
                         "EPT lifecycle: table contents changed");
            inspectAttributes = {};
            inspectResponse = {};
            squash = {};
            awaiting = false;
            // Check after PagingPort releases its executing callback, not
            // inside it. A leaked walk/callback would retain the predicate.
            schedule(new EventFunctionWrapper([this, lifetime] {
                panic_if(!lifetime.expired() ||
                         !tlb->getWalker()->pagingPort().idle(),
                         "EPT lifecycle: retained state after completion");
                next();
            }, name() + ".ept_retired", true), curTick() + 1);
        });
    if (stop) {
        panic_if(!awaiting || port.outstanding() != 1,
                 "EPT lifecycle: request lost before completion");
    }
    if (retry && stop == 1) {
        // The root read is rejected and waiting for retry. Functional EPT
        // must still use the same port without consuming its pending owner.
        auto inspect = std::move(inspectAttributes);
        const auto functional = ept::walkFunctional(port, requestor,
            ept::LookupInput(Virtual, EptAccess::Read, config), 48, Caps);
        inspectAttributes = std::move(inspect);
        checkResult(functional, expected, "functional during retry");
        panic_if(!awaiting || port.outstanding() != 1 || curTick() != start,
                 "Functional EPT disturbed queued timing lookup");
    }
    // The caller's configuration and temporary LookupInput do not survive
    // this function. Outstanding reads must use their own captured copy.
    config = {};
}

void
PagingTester::eptTimingConcurrent()
{
    setup(Long);
    constexpr uint64_t Caps = (1ULL << 6) | (1ULL << 8) | (1ULL << 14);
    constexpr Addr Delta = 0x10000;
    const auto configA = *ept::decodeEptp(Tables | (3 << 3) | 6, Caps, 48);
    const auto configB = *ept::decodeEptp(
        (Tables + Delta) | (3 << 3), Caps, 48);
    descriptor(3, descriptor(3) | (6 << 3));
    const auto slots = descriptors;
    std::array<uint64_t, 4> entriesA, entriesB;
    for (unsigned i = 0; i < 4; ++i) {
        entriesA[i] = descriptor(i);
        entriesB[i] = i == 3 ? (Physical + 0x100000) | 7 | (6 << 3) :
                              entriesA[i] + Delta;
        system->physProxy.write<uint64_t>(slots[i] + Delta, entriesB[i],
                                         ByteOrder::little);
    }
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    const ept::LookupInput inputA(Virtual, EptAccess::Read, configA);
    const ept::LookupInput inputB(Virtual, EptAccess::Write, configB);
    const auto expectedA = ept::walkFunctional(
        port, requestor, inputA, 48, Caps);
    const auto expectedB = ept::walkFunctional(
        port, requestor, inputB, 48, Caps);
    struct Progress
    {
        std::array<unsigned, 3> responses{};
        std::array<bool, 3> done{};
        unsigned completed = 0;
    };
    auto progress = std::make_shared<Progress>();
    auto valid = std::make_shared<bool>(true);
    std::weak_ptr<bool> lifetime = valid;
    inspectAttributes = [=](PacketPtr packet) {
        const bool second = packet->getAddr() >= Tables + Delta;
        const unsigned client = second ? 1 : progress->done[0] ? 2 : 0;
        const unsigned level = progress->responses[client];
        panic_if(level >= 4 || packet->cmd != MemCmd::ReadReq ||
                 packet->getSize() != 8 || packet->getAddr() !=
                     slots[level] + (second ? Delta : 0) ||
                 packet->req->isUncacheable() != second,
                 "Concurrent EPT lookup mixed roots, attributes or levels");
    };
    inspectResponse = [=](PacketPtr packet) {
        const bool second = packet->getAddr() >= Tables + Delta;
        const unsigned client = second ? 1 : progress->done[0] ? 2 : 0;
        const unsigned level = progress->responses[client]++;
        panic_if(level >= 4 || packet->getAddr() !=
                     slots[level] + (second ? Delta : 0),
                 "Concurrent EPT response was routed to the wrong lookup");
    };
    const Tick start = curTick();
    const auto finish = [=, this](unsigned client,
                                  const ept::WalkResult &result) {
        panic_if(progress->done[client] || progress->responses[client] != 4 ||
                 curTick() <= start || !awaiting ||
                 tlb->getWalker()->pagingPort().idle(),
                 "Concurrent EPT completion lost ownership or repeated");
        checkResult(result, client == 1 ? expectedB : expectedA, "concurrent");
        progress->done[client] = true;
        if (++progress->completed != 3)
            return;
        for (unsigned i = 0; i < 4; ++i) {
            panic_if(descriptor(i) != entriesA[i] ||
                     system->physProxy.read<uint64_t>(slots[i] + Delta,
                         ByteOrder::little) != entriesB[i],
                     "Concurrent EPT lookup modified a table");
        }
        inspectAttributes = {};
        inspectResponse = {};
        awaiting = false;
        schedule(new EventFunctionWrapper([this, lifetime] {
            panic_if(!lifetime.expired() ||
                     !tlb->getWalker()->pagingPort().idle(),
                     "Concurrent EPT retained state after retirement");
            next();
        }, name() + ".ept_concurrent_retired", true), curTick() + 1);
    };
    awaiting = true;
    injection = RetryRead;
    ept::walkTiming(port, requestor, inputA, 48, Caps,
        [valid] { return *valid; },
        [=, this](const ept::WalkResult &result) {
            finish(0, result);
            // Reenter while the original completion is still port-owned.
            ept::walkTiming(tlb->getWalker()->pagingPort(), requestor,
                inputA, 48, Caps, [valid] { return *valid; },
                [finish](const ept::WalkResult &nested) {
                    finish(2, nested);
                });
        });
    ept::walkTiming(port, requestor, inputB, 48, Caps,
        [valid] { return *valid; },
        [finish](const ept::WalkResult &result) { finish(1, result); });
    panic_if(port.outstanding() != 2 || progress->completed,
             "Independent EPT lookups entered a guest admission queue");
}

} // namespace gem5::X86ISA
