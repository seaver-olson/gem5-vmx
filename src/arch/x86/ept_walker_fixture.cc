/* SPDX-License-Identifier: BSD-3-Clause */
#include <algorithm>
#include <array>

#include "arch/x86/ept_walker.hh"
#include "arch/x86/faults.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/ldstflags.hh"
#include "arch/x86/mmu.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/paging.hh"
#include "arch/x86/paging_tester.hh"
#include "arch/x86/tlb.hh"
#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "mem/port_proxy.hh"
#include "sim/system.hh"

namespace gem5::X86ISA
{
namespace
{

constexpr Addr EptRoot = 0x2800000;
constexpr Addr EptTableLeaves = EptRoot + 3 * 4096;
constexpr Addr HostTables = 0x3000000;
constexpr Addr HostData = 0x3200000;
constexpr Addr OtherHostData = 0x3400000;
constexpr Addr EptDataLeaf = EptRoot + 8192 + 8 * (0x2000000 >> 21);
constexpr uint64_t NestedCaps = 1 | (1ULL << 6) | (1ULL << 8) |
    (1ULL << 14) | (1ULL << 16);

class EptTranslation : public BaseMMU::Translation
{
  public:
    using Callback = std::function<void(const Fault &, const RequestPtr &,
                                       ThreadContext *, BaseMMU::Mode)>;
    explicit EptTranslation(Callback callback,
                            std::shared_ptr<bool> cancelled = nullptr)
        : callback(std::move(callback)), cancelled(std::move(cancelled)) {}
    bool squashed() const override { return cancelled && *cancelled; }
    void markDelayed() override
    {
        panic_if(finished || delayed, "EPT MMU marked delayed after completion");
        delayed = true;
    }
    void finish(const Fault &fault, const RequestPtr &req,
                ThreadContext *tc, BaseMMU::Mode mode) override
    {
        panic_if(finished, "EPT MMU completed a request twice");
        finished = true;
        callback(fault, req, tc, mode);
    }
  private:
    Callback callback;
    std::shared_ptr<bool> cancelled;
    bool finished = false;
    bool delayed = false;
};

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
PagingTester::setupEpt(bool presetAD, bool eptAD)
{
    setup(Long);
    std::array<uint8_t, 4 * 4096> zero{};
    system->physProxy.writeBlob(EptRoot, zero.data(), zero.size());
    system->physProxy.writeBlob(HostTables, zero.data(), zero.size());
    system->physProxy.write<uint64_t>(EptRoot,
        (EptRoot + 4096) | 7, ByteOrder::little);
    system->physProxy.write<uint64_t>(EptRoot + 4096,
        (EptRoot + 8192) | 7, ByteOrder::little);
    system->physProxy.write<uint64_t>(EptRoot + 8192 + 8 * (Tables >> 21),
        EptTableLeaves | 7, ByteOrder::little);
    system->physProxy.write<uint64_t>(EptDataLeaf,
        HostData | 7 | (6ULL << 3) | 128, ByteOrder::little);
    for (unsigned level = 0; level < descriptors.size(); ++level) {
        const auto value = descriptor(level) | (presetAD ? 32 : 0) |
            (presetAD && level == 3 ? 64 : 0);
        const Addr host = HostTables + level * 4096 +
            (descriptors[level] & 4095);
        descriptor(level, 0); // Direct GPA reads must not accidentally work.
        descriptors[level] = host;
        descriptor(level, value);
        system->physProxy.write<uint64_t>(EptTableLeaves + 8 * level,
            (HostTables + level * 4096) | 7 | (6ULL << 3), ByteOrder::little);
    }
    const Addr offset = Virtual & 4095;
    system->physProxy.write<uint8_t>(Physical + offset, 0xcc);
    system->physProxy.write<uint8_t>(HostData + offset, 0x90);
    system->physProxy.write<uint8_t>(HostData + 4096 + offset, 0xeb);
    system->physProxy.write<uint8_t>(OtherHostData + offset, 0xeb);
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    panic_if(isa->vmxState().activeEptConfig,
             "EPT fixture overwrote an active EPT context");
    savedEptCapability = tc->readMiscRegNoEffect(misc_reg::VmxEptVpidCap);
    const uint64_t caps = NestedCaps | (eptAD ? 1ULL << 21 : 0);
    tc->setMiscRegNoEffect(misc_reg::VmxEptVpidCap, caps);
    isa->vmxState().activeEptConfig =
        *ept::decodeEptp(EptRoot | (3 << 3) | 6 | (eptAD ? 64 : 0), caps, 48);
}

void
PagingTester::finishEpt()
{
    static_cast<ISA *>(tc->getIsaPtr())->vmxState().activeEptConfig.reset();
    tc->setMiscRegNoEffect(misc_reg::VmxEptVpidCap, savedEptCapability);
    inspectAttributes = {};
    inspectResponse = {};
    squash = {};
    awaiting = false;
    schedule(nextEvent, curTick() + 1);
}

void
PagingTester::eptMmuCases()
{
    for (unsigned size : {1, 2, 4, 8}) {
        cases.emplace_back([=, this] { eptUncachedSwap(size, false, true); });
        if (size >= 4) {
            for (bool match : {false, true}) {
                cases.emplace_back(
                    [=, this] { eptUncachedSwap(size, true, match); });
            }
        }
    }
    if (system->isTimingMode() && system->threads.size() > 1) {
        for (bool locked : {false, true}) {
            cases.emplace_back([=, this] { eptUncachedPair(locked); });
        }
    }
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        for (auto mode : {BaseMMU::Read, BaseMMU::Write, BaseMMU::Execute}) {
            for (unsigned path = 0; path < 3; ++path) {
                cases.emplace_back([=, this] {
                    eptMemoryTypeCase(scenario, mode, path == 2, path == 1);
                });
            }
        }
    }
    for (auto mode : {BaseMMU::Read, BaseMMU::Write, BaseMMU::Execute}) {
        for (bool functional : {false, true}) {
            for (bool warm : {false, true}) {
                if (functional && warm) // Functional deliberately bypasses TLBs.
                    continue;
                for (auto test : {EptTest::Mapping, EptTest::DataMisconfig,
                     mode == BaseMMU::Read ? EptTest::DeniedRead :
                     mode == BaseMMU::Write ? EptTest::DeniedWrite :
                                             EptTest::DeniedExecute}) {
                    cases.emplace_back([=, this] {
                        eptMmuAccess(mode, functional, warm, test);
                    });
                }
            }
        }
    }
    for (bool functional : {false, true}) {
        for (unsigned level = 0; level < 4; ++level) {
            for (auto test : {EptTest::TableRead, EptTest::TableMisconfig,
                              EptTest::TableUpdate}) {
                if (functional && test == EptTest::TableUpdate)
                    continue;
                cases.emplace_back([=, this] {
                    eptMmuAccess(BaseMMU::Write, functional, false, test, level);
                });
            }
        }
        cases.emplace_back([=, this] {
            eptMmuAccess(BaseMMU::Read, functional, false, EptTest::GuestFault);
        });
    }
    cases.emplace_back([this] {
        eptMmuAccess(BaseMMU::Write, false, false, EptTest::Replace);
    });
    cases.emplace_back([this] {
        eptMmuAccess(BaseMMU::Read, false, true, EptTest::Remap);
    });
    for (bool warm : {false, true}) {
        for (auto test : {EptTest::InheritedWrite, EptTest::DirtyUpdate,
                          EptTest::Rmw}) {
            cases.emplace_back([=, this] {
                eptMmuAccess(test == EptTest::Rmw ? BaseMMU::Read :
                    BaseMMU::Write, false, warm, test, 3);
            });
        }
    }
    if (system->isTimingMode()) {
        for (unsigned point = 0; point < 4; ++point) {
            for (auto inject : {RetryRead, SquashRetryRead, SquashRead,
                                 InvalidateRead}) {
                cases.emplace_back([=, this] { eptMmuTiming(point, inject); });
            }
        }
        for (auto inject : {RetryRead, SquashRetryRead, SquashRead,
                             InvalidateRead}) {
            cases.emplace_back([=, this] { eptMmuTiming(2, inject, true); });
        }
        cases.emplace_back([this] { eptMmuTiming(4, SquashRead); });
    }
    for (bool cd : {false, true}) {
        for (auto test :
             {EptAdTest::Read, EptAdTest::Write, EptAdTest::Execute,
              EptAdTest::Large2M, EptAdTest::Large1G, EptAdTest::FiveLevel,
              EptAdTest::AlreadySet, EptAdTest::Replace, EptAdTest::Revoke,
              EptAdTest::Denied, EptAdTest::Absent, EptAdTest::Misconfig,
              EptAdTest::UpdateError}) {
            cases.emplace_back([=, this] { eptAdCase(test, cd); });
        }
        if (system->isTimingMode()) {
            for (auto test : {EptAdTest::Retry, EptAdTest::CancelQueued,
                              EptAdTest::CancelIssued}) {
                cases.emplace_back([=, this] { eptAdCase(test, cd); });
            }
        }
        for (bool denied : {false, true}) {
            for (bool functional : {false, true}) {
                cases.emplace_back([=, this] {
                    eptAdGuest(denied, functional, false, true, cd);
                });
            }
        }
        cases.emplace_back(
            [=, this] { eptAdGuest(false, false, false, false, cd); });
    }
}

void
PagingTester::eptMemoryTypeCase(unsigned scenario, BaseMMU::Mode mode,
                                bool functional, bool warm, bool primed)
{
    using Type = Request::MemoryType;
    const uint8_t ept_types[] = {6, 6, 6, 1, 4, 5, 0, 5};
    const Type expected[] = {Type::Uncacheable,    Type::WriteBack,
                             Type::Uncacheable,    Type::WriteCombining,
                             Type::WriteThrough,   Type::WriteProtected,
                             Type::WriteCombining, Type::WriteCombining};
    const bool cd = scenario == 2;
    const bool ignore = scenario == 1;
    const unsigned selector = scenario == 1 ? 3 : scenario == 7 ? 2 : 0;
    uint64_t pat = 0x0007040600070406ULL;
    if (scenario == 0 || scenario == 6) {
        pat = (pat & ~0xffULL) | (scenario == 6 ? 1 : 0);
    }
    if (!primed) {
        setupEpt(true);
        CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
        cr0.cd = cd;
        tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
        tc->setMiscRegNoEffect(misc_reg::Pat, pat);
        tc->setMiscRegNoEffect(misc_reg::Cr3, Tables | (ignore ? 24 : 0));
        for (unsigned level = 0; level < 4; ++level) {
            const unsigned attributes = level == 3 ? selector : level + 1;
            descriptor(level, descriptor(level) | ((attributes & 3) << 3));
            if (ignore) {
                const Addr address = EptTableLeaves + 8 * level;
                auto leaf = system->physProxy.read<uint64_t>(
                    address, ByteOrder::little);
                system->physProxy.write<uint64_t>(address, leaf | 64,
                                                  ByteOrder::little);
            }
        }
        system->physProxy.write<uint64_t>(
            EptDataLeaf,
            HostData | 7 | (uint64_t(ept_types[scenario]) << 3) | 128 |
                (ignore ? 64 : 0),
            ByteOrder::little);
    }
    auto *cache =
        static_cast<TLB *>(mode == BaseMMU::Execute ? mmu->itb : mmu->dtb);
    panic_if(bool(cache->lookup(Virtual, false, {tc->contextId(), 0})) !=
                 primed,
             "EPT memory-type case missed its intended TLB path");
    // The existing interposer observes the data walker only. Classic's
    // shared I/D TLB exposes fetch packets too; Ruby's independent I-walker
    // is still checked by its returned type and captured-state assertions.
    const unsigned expected_tables =
        primed || (mode == BaseMMU::Execute && mmu->itb != mmu->dtb) ? 0 : 4;
    auto counts = std::make_shared<std::array<unsigned, 2>>();
    inspectAttributes = [=](PacketPtr packet) {
        const bool guest = packet->getAddr() >= HostTables &&
                           packet->getAddr() < HostTables + 4 * 4096;
        ++(*counts)[guest];
        const auto attrs = PagingPort::attributes(packet);
        const auto type =
            guest ? paging::effectiveEptType(
                        cd, 6, ignore,
                        paging::patType(pat, unsigned(attrs.pwt) |
                                                 (unsigned(attrs.pcd) << 1)))
                  : (cd ? Type::Uncacheable : Type::WriteBack);
        panic_if(
            packet->req->memoryType() != type ||
                packet->req->isUncacheable() != (type == Type::Uncacheable ||
                                                 type == Type::WriteCombining),
            "EPT memory-type case %u: wrong table request type", scenario);
    };
    auto flags = accessFlags;
    if (mode == BaseMMU::Execute) {
        flags.set(Request::INST_FETCH);
    }
    auto req = std::make_shared<Request>(Virtual, 1, flags,
                                         system->getRequestorId(this), Virtual,
                                         tc->contextId());
    const Tick start = curTick();
    const auto verify = [=, this](const Fault &fault, const RequestPtr &out,
                                  ThreadContext *context,
                                  BaseMMU::Mode returned) {
        Request policy(0, 1, 0, req->requestorId());
        paging::applyMemoryType(policy, expected[scenario],
                                mode == BaseMMU::Write);
        panic_if(fault || out != req || context != tc || returned != mode ||
                     out->getPaddr() != HostData + (Virtual & 4095) ||
                     out->memoryType() != expected[scenario] ||
                     out->isUncacheable() != policy.isUncacheable() ||
                     out->isStrictlyOrdered() != policy.isStrictlyOrdered() ||
                     (*counts)[1] != expected_tables ||
                     (functional && curTick() != start),
                 "EPT memory-type case %u: wrong final type or captured state",
                 scenario);
        CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
        cr0.cd = cd;
        tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
        tc->setMiscRegNoEffect(misc_reg::Pat, pat);
        if (warm && !primed) {
            schedule(new EventFunctionWrapper(
                         [=, this] {
                             eptMemoryTypeCase(scenario, mode, false, true,
                                               true);
                         },
                         name() + ".ept_type_warm", true),
                     curTick() + 1);
        } else {
            // Exercise the resolved policy on a real final memory request,
            // not only on the MMU's returned flags. A WB readback must see
            // the write, including across a second coherent cache endpoint.
            inspectAttributes = {};
            auto *packet = mode == BaseMMU::Write ? Packet::createWrite(out)
                                                  : Packet::createRead(out);
            packet->allocate();
            if (mode == BaseMMU::Write) {
                packet->setLE<uint8_t>(0xa5);
            }
            auto &physical = tlb->getWalker()->pagingPort();
            const auto accessed = [=, this](PacketPtr response,
                                            bool cancelled) {
                panic_if(cancelled || response->isError() ||
                             (mode != BaseMMU::Write &&
                              response->getLE<uint8_t>() != 0x90),
                         "EPT memory-type case %u: wrong final data",
                         scenario);
                delete response;
                auto req = std::make_shared<Request>(
                    out->getPaddr(), 1, Request::PHYSICAL, out->requestorId());
                req->memoryType(Type::WriteBack);
                auto *readback = Packet::createRead(req);
                readback->allocate();
                const auto checked = [=, this](PacketPtr pkt, bool cancelled) {
                    panic_if(cancelled || pkt->isError() ||
                                 pkt->getLE<uint8_t>() !=
                                     (mode == BaseMMU::Write ? 0xa5 : 0x90),
                             "EPT memory-type case %u: incoherent readback",
                             scenario);
                    delete pkt;
                    finishEpt();
                };
                auto *peer = &tlb->getWalker()->pagingPort();
                if (system->threads.size() > 1) {
                    auto *other =
                        static_cast<MMU *>(system->threads[1]->getMMUPtr());
                    peer = &other->getDataWalker()->pagingPort();
                }
                if (functional) {
                    peer->sendFunctional(readback);
                    checked(readback, false);
                } else if (system->isTimingMode()) {
                    peer->submit(readback, [] { return true; }, checked);
                } else {
                    peer->sendAtomic(readback);
                    checked(readback, false);
                }
            };
            if (functional) {
                physical.sendFunctional(packet);
                accessed(packet, false);
            } else if (system->isTimingMode()) {
                physical.submit(packet, [] { return true; }, accessed);
            } else {
                physical.sendAtomic(packet);
                accessed(packet, false);
            }
        }
    };
    awaiting = true;
    if (functional) {
        verify(mmu->translateFunctional(req, tc, mode), req, tc, mode);
    } else if (system->isTimingMode()) {
        auto callback = std::make_unique<EptTranslation>(verify);
        auto *raw = callback.get();
        continuations.push_back(std::move(callback));
        mmu->translateTiming(req, tc, raw, mode);
        // Mutate live registers after admission; every stage must use the
        // captured PAT/CD values, including the final EPT continuation.
        tc->setMiscRegNoEffect(misc_reg::Pat, 0);
        CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
        cr0.cd = !cd;
        tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
    } else {
        verify(mmu->translateAtomic(req, tc, mode), req, tc, mode);
    }
}

void
PagingTester::eptUncachedSwap(unsigned size, bool conditional, bool match)
{
    constexpr Addr Address = HostData + 8;
    constexpr uint64_t Old = 0x1020304050607080;
    constexpr uint64_t New = 0xfedcba9876543210;
    constexpr uint64_t Neighbor = 0x8877665544332211;
    setup(Long);
    system->physProxy.write<uint64_t>(Address + 8, Neighbor,
                                      ByteOrder::little);
    const auto requestor = system->getRequestorId(this);
    auto &port = tlb->getWalker()->pagingPort();
    auto *peer = &port;
    if (system->threads.size() > 1) {
        auto *other = static_cast<MMU *>(system->threads[1]->getMMUPtr());
        peer = &other->getDataWalker()->pagingPort();
    }
    auto seed_req =
        std::make_shared<Request>(Address, 8, Request::PHYSICAL, requestor);
    seed_req->memoryType(Request::MemoryType::WriteBack);
    auto *seed = Packet::createWrite(seed_req);
    seed->allocate();
    seed->setLE<uint64_t>(Old);
    const auto seeded = [=, this](PacketPtr response, bool cancelled) {
        panic_if(cancelled || response->isError(), "Swap seed failed");
        delete response;
        auto req = std::make_shared<Request>(
            Address, size,
            Request::PHYSICAL |
                (conditional ? Request::MEM_SWAP_COND : Request::MEM_SWAP),
            requestor);
        paging::applyMemoryType(*req, paging::MemoryType::Uncacheable, true);
        if (conditional) {
            req->setExtraData((Old & mask(size * 8)) ^ !match);
        }
        auto *swap = new Packet(req, MemCmd::SwapReq);
        swap->allocate();
        swap->setUintX(New, ByteOrder::little);
        const auto checked = [=, this](PacketPtr pkt, bool cancelled) {
            const uint64_t expected =
                !conditional || match
                    ? (Old & ~mask(size * 8)) | (New & mask(size * 8))
                    : Old;
            panic_if(cancelled || pkt->isError() ||
                         pkt->getUintX(ByteOrder::little) !=
                             (Old & mask(size * 8)) ||
                         system->physProxy.read<uint64_t>(
                             Address, ByteOrder::little) != expected ||
                         system->physProxy.read<uint64_t>(
                             Address + 8, ByteOrder::little) != Neighbor,
                     "UC swap returned wrong old data or corrupted its line");
            delete pkt;
            awaiting = false;
            schedule(nextEvent, curTick() + 1);
        };
        auto &physical = tlb->getWalker()->pagingPort();
        if (system->isTimingMode()) {
            physical.submit(swap, [] { return true; }, checked);
        } else {
            physical.sendAtomic(swap);
            checked(swap, false);
        }
    };
    awaiting = true;
    if (system->isTimingMode()) {
        peer->submit(seed, [] { return true; }, seeded);
    } else {
        peer->sendAtomic(seed);
        seeded(seed, false);
    }
}

void
PagingTester::eptUncachedPair(bool lockedPeer)
{
    constexpr Addr Address = HostData;
    constexpr uint64_t Old = 0x1122334455667788;
    constexpr uint64_t Other = 0xaabbccddeeff0011;
    constexpr uint64_t Entry = 0x12345007;
    setup(Long);
    system->physProxy.write<uint64_t>(Address + 8, Other, ByteOrder::little);
    system->physProxy.write<uint64_t>(Address + 64, Entry, ByteOrder::little);
    auto *port = &tlb->getWalker()->pagingPort();
    auto *other = static_cast<MMU *>(system->threads[1]->getMMUPtr());
    auto *peer = &other->getDataWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    const auto packet = [=](Addr address, MemCmd cmd, Request::Flags flags) {
        auto req = std::make_shared<Request>(
            address, 8, flags | Request::PHYSICAL, requestor);
        paging::applyMemoryType(*req,
                                flags.isSet(Request::LOCKED_RMW) ||
                                        flags.isSet(Request::ATOMIC_RETURN_OP)
                                    ? paging::MemoryType::Uncacheable
                                    : paging::MemoryType::WriteBack,
                                true);
        auto *pkt = new Packet(req, cmd);
        pkt->allocate();
        return pkt;
    };
    auto *seed = packet(Address, MemCmd::WriteReq, 0);
    seed->setLE<uint64_t>(Old);
    awaiting = true;
    peer->submit(
        seed, [] { return true; },
        [=, this](PacketPtr pkt, bool cancel) {
            panic_if(cancel || pkt->isError(), "Locked seed failed");
            delete pkt;
            port->submit(
                packet(Address, MemCmd::LockedRMWReadReq, Request::LOCKED_RMW),
                [] { return true; },
                [=, this](PacketPtr pkt, bool cancel) {
                    panic_if(cancel || pkt->isError() ||
                                 pkt->getLE<uint64_t>() != Old,
                             "Uncached locked read lost a dirty owner");
                    delete pkt;
                    system->physProxy.write<uint64_t>(Address + 8, ~Other,
                                                      ByteOrder::little);
                    auto completed =
                        std::make_shared<std::array<unsigned, 2>>();
                    const auto done = [=, this](unsigned endpoint,
                                                PacketPtr pkt,
                                                bool cancelled) {
                        panic_if(cancelled || pkt->isError() ||
                                     ++(*completed)[endpoint] != 1,
                                 "Uncached locked pair did not retire exactly "
                                 "once");
                        delete pkt;
                        if ((*completed)[0] && (*completed)[1]) {
                            panic_if(
                                system->physProxy.read<uint64_t>(
                                    Address, ByteOrder::little) != Other ||
                                    system->physProxy.read<uint64_t>(
                                        Address + 8, ByteOrder::little) !=
                                        ~Other,
                                "Locked write lost a peer or adjacent bytes");
                            awaiting = false;
                            schedule(nextEvent, curTick() + 1);
                        }
                    };
                    auto *contender =
                        packet(Address,
                               lockedPeer ? MemCmd::LockedRMWReadReq
                                          : MemCmd::WriteReq,
                               lockedPeer ? Request::Flags(Request::LOCKED_RMW)
                                          : Request::Flags());
                    contender->setLE<uint64_t>(Other);
                    peer->submit(
                        contender, [] { return true; },
                        [=](PacketPtr pkt, bool cancelled) {
                            if (!lockedPeer) {
                                done(1, pkt, cancelled);
                                return;
                            }
                            panic_if(cancelled || pkt->isError() ||
                                         pkt->getLE<uint64_t>() != Old + 1,
                                     "Competing locked read preceded unlock");
                            delete pkt;
                            auto *write =
                                packet(Address, MemCmd::LockedRMWWriteReq,
                                       Request::LOCKED_RMW);
                            write->setLE<uint64_t>(Other);
                            peer->submit(
                                write, [] { return true; },
                                [=](PacketPtr pkt, bool cancelled) {
                                    done(1, pkt, cancelled);
                                });
                        });
                    // A real A/D update on another HPA must still make
                    // progress while the target line remains exclusively held
                    // by the CPU.
                    auto *update = packet(Address + 64, MemCmd::SwapReq,
                                          Request::ATOMIC_RETURN_OP);
                    update->req->setExtraData(Entry);
                    update->req->setAtomicOpFunctor(
                        std::make_unique<paging::ConditionalUpdate<uint64_t>>(
                            Entry, 32));
                    port->submit(
                        update, [] { return true; },
                        [=, this](PacketPtr pkt, bool cancelled) {
                            panic_if(
                                cancelled || pkt->isError() ||
                                    pkt->getLE<uint64_t>() != Entry ||
                                    (*completed)[1] ||
                                    system->physProxy.read<uint64_t>(
                                        Address, ByteOrder::little) != Old ||
                                    system->physProxy.read<uint64_t>(
                                        Address + 64, ByteOrder::little) !=
                                        (Entry | 32),
                                "Locked ownership blocked paging or admitted "
                                "a peer");
                            delete pkt;
                            auto *write =
                                packet(Address, MemCmd::LockedRMWWriteReq,
                                       Request::LOCKED_RMW);
                            write->setLE<uint64_t>(Old + 1);
                            port->submit(
                                write, [] { return true; },
                                [=](PacketPtr pkt, bool cancelled) {
                                    done(0, pkt, cancelled);
                                });
                        });
                });
        });
}

void
PagingTester::eptAdCase(EptAdTest test, bool cacheDisabled)
{
    setup(Long);
    constexpr uint64_t A = 1ULL << 8, D = 1ULL << 9;
    constexpr uint64_t Software = (1ULL << 11) | (1ULL << 52) | (1ULL << 63);
    constexpr uint64_t Caps = NestedCaps | (1ULL << 7) | (1ULL << 17) |
                              (1ULL << 21);
    const unsigned levels = test == EptAdTest::FiveLevel ? 5 : 4;
    const unsigned leaf = test == EptAdTest::Large2M ? 2 :
                          test == EptAdTest::Large1G ? 3 : 1;
    const auto access = test == EptAdTest::Execute ? EptAccess::Execute :
        test == EptAdTest::Read || test == EptAdTest::Replace ||
        test == EptAdTest::FiveLevel ? EptAccess::Read : EptAccess::Write;
    const Addr gpa = 0x123456789abcULL |
        (levels == 5 ? 1ULL << 48 : 0);
    // Reuse the same real pages across memory types to cover cached aliases.
    const Addr root = 0x1800000 + unsigned(test) * 0x10000;
    std::array<uint8_t, 5 * 4096> zero{};
    system->physProxy.writeBlob(root, zero.data(), zero.size());
    descriptors.clear();
    for (unsigned level = levels; level >= leaf; --level) {
        const Addr table = root + (levels - level) * 4096;
        descriptors.push_back(table +
            8 * ((gpa >> (12 + 9 * (level - 1))) & 511));
        uint64_t raw = level == leaf ?
            0x80000000ULL | 7 | (6 << 3) | (leaf > 1 ? 128 : 0) :
            (table + 4096) | 7 | D; // Non-leaf bit 9 is ignored; preserve it.
        raw |= Software;
        if (test == EptAdTest::AlreadySet)
            raw |= A | (level == leaf ? D : 0);
        descriptor(descriptors.size() - 1, raw);
    }
    if (test == EptAdTest::Denied || test == EptAdTest::Misconfig ||
        test == EptAdTest::Absent)
        descriptor(descriptors.size() - 1,
            (descriptor(descriptors.size() - 1) & ~7ULL) |
            (test == EptAdTest::Denied ? 5 :
             test == EptAdTest::Misconfig ? 2 : 0));
    const auto config = *ept::decodeEptp(root | ((levels - 1) << 3) | 64 |
        (test == EptAdTest::Write ? 0 : 6), Caps, 48);
    const ept::LookupInput input(gpa, access, config, false, cacheDisabled);
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    std::vector<uint64_t> before;
    for (unsigned i = 0; i < descriptors.size(); ++i)
        before.push_back(descriptor(i));
    const Tick functionalStart = curTick();
    const auto functional = ept::walkFunctional(port, requestor, input, 48, Caps);
    const auto initialStatus = test == EptAdTest::Misconfig ?
        ept::WalkStatus::Misconfiguration :
        test == EptAdTest::Denied || test == EptAdTest::Absent ?
        ept::WalkStatus::Violation : ept::WalkStatus::Mapping;
    panic_if(functional.status != initialStatus,
             "Functional EPT A/D returned an incorrect status");
    for (unsigned i = 0; i < before.size(); ++i)
        panic_if(descriptor(i) != before[i], "Functional EPT A/D changed memory");
    panic_if(curTick() != functionalStart, "Functional EPT A/D advanced time");

    struct Progress { unsigned updates = 0, callbacks = 0; bool armed = false; };
    auto progress = std::make_shared<Progress>();
    auto cancelled = std::make_shared<bool>(false);
    squash = [cancelled] { *cancelled = true; };
    inspectAttributes = [=, this](PacketPtr packet) {
        const auto slot = std::find(descriptors.begin(), descriptors.end(),
                                    packet->getAddr());
        panic_if(pendingPackets || slot == descriptors.end() ||
                     packet->getSize() != 8 ||
                     packet->req->getPaddr() != packet->getAddr() ||
                     !packet->req->getFlags().isSet(Request::PHYSICAL) ||
                     packet->req->isUncacheable() !=
                         (cacheDisabled || config.memoryType == 0) ||
                     packet->req->memoryType() !=
                         (cacheDisabled || config.memoryType == 0
                              ? Request::MemoryType::Uncacheable
                              : Request::MemoryType::WriteBack),
                 "EPT A/D sent an incorrect physical request");
        const bool update = packet->req->isAtomicReturn();
        panic_if(packet->cmd != (update ? MemCmd::SwapReq : MemCmd::ReadReq),
                 "EPT A/D used an incorrect memory command");
        if (!update)
            return;
        ++progress->updates;
        if (packet->getAddr() != descriptors.back() || progress->armed)
            return;
        progress->armed = true;
        if (test == EptAdTest::Read && cacheDisabled &&
            system->isTimingMode()) {
            const auto slots = descriptors;
            // Inspect real coherent state while an uncached conditional
            // update is in flight, not just after its response. A and D
            // may advance; descriptor addresses/rights/software bits may not.
            for (Tick delay : {1, 20000, 40000, 60000, 80000, 120000}) {
                schedule(new EventFunctionWrapper(
                             [=, this] {
                                 if (progress->callbacks) {
                                     return;
                                 }
                                 for (unsigned i = 0; i < slots.size(); ++i) {
                                     const auto value =
                                         system->physProxy.read<uint64_t>(
                                             slots[i], ByteOrder::little);
                                     panic_if((value & ~(A | D)) !=
                                                  (before[i] & ~(A | D)),
                                              "Functional read at %#x "
                                              "returned %#x; expected %#x "
                                              "apart from A/D",
                                              slots[i], value, before[i]);
                                 }
                             },
                             name() + ".ept_ad_functional", true),
                         curTick() + delay);
            }
        }
        if (test == EptAdTest::Replace || test == EptAdTest::Revoke) {
            // Change the root rights as well: resuming at the old leaf would
            // return stale permissions even if it noticed the new PFN.
            const uint64_t root_value = descriptor(0) & ~2ULL;
            const uint64_t leaf_value = 0xc0000000ULL | 7 | (6 << 3) |
                                        Software |
                                        (test == EptAdTest::Replace ? D : 0);
            if (system->isTimingMode() && system->threads.size() > 1) {
                // Hold the observed update while a separate coherent cache
                // endpoint replaces descriptors using real WriteReqs.
                auto *peer_mmu =
                    static_cast<MMU *>(system->threads[1]->getMMUPtr());
                auto *peer_port = &peer_mmu->getDataWalker()->pagingPort();
                const auto write = [=](Addr address, uint64_t value,
                                       PagingPort::Completion done) {
                    auto req = std::make_shared<Request>(
                        address, 8, Request::PHYSICAL, requestor);
                    auto *pkt = new Packet(req, MemCmd::WriteReq);
                    pkt->allocate();
                    pkt->setLE<uint64_t>(value);
                    peer_port->submit(
                        pkt, [] { return true; }, std::move(done));
                };
                const Addr root_address = descriptors.front();
                const Addr leaf_address = descriptors.back();
                startPeer = [=, this] {
                    write(root_address, root_value,
                          [=, this](PacketPtr pkt, bool cancelled) {
                              panic_if(cancelled || pkt->isError(),
                                       "Coherent EPT root replacement failed");
                              delete pkt;
                              write(leaf_address, leaf_value,
                                    [this](PacketPtr pkt, bool cancelled) {
                                        panic_if(cancelled || pkt->isError(),
                                                 "Coherent EPT leaf "
                                                 "replacement failed");
                                        delete pkt;
                                        schedule(retryEvent, curTick() + 1);
                                    });
                          });
                };
                injection = PeerUpdate;
            } else {
                descriptor(0, root_value);
                descriptor(descriptors.size() - 1, leaf_value);
            }
        } else if (test == EptAdTest::UpdateError && system->isAtomicMode()) {
            packet->makeResponse();
            packet->setBadAddress();
        } else if (test == EptAdTest::Retry) {
            injection = RetryUpdate;
        } else if (test == EptAdTest::CancelQueued) {
            injection = SquashRetryUpdate;
        } else if (test == EptAdTest::CancelIssued) {
            injection = SquashUpdate;
        }
    };
    inspectResponse = [=, this](PacketPtr packet) {
        if ((test == EptAdTest::UpdateError || *cancelled) &&
            packet->req->isAtomicReturn() &&
            packet->getAddr() == descriptors.back())
            packet->setBadAddress();
    };
    const auto verify = [=, this](const ept::WalkResult &result, Tick latency) {
        const bool cancel = test == EptAdTest::CancelQueued ||
                            test == EptAdTest::CancelIssued;
        const auto status = cancel ? ept::WalkStatus::Cancelled :
            test == EptAdTest::UpdateError ? ept::WalkStatus::MemoryError :
            test == EptAdTest::Revoke ? ept::WalkStatus::Violation :
            functional.status;
        panic_if(++progress->callbacks != 1 || result.status != status ||
                 (system->isAtomicMode() && latency != observedAtomicLatency),
                 "EPT A/D returned wrong status, latency or callback count");
        if (status == ept::WalkStatus::Mapping) {
            const Addr base = test == EptAdTest::Replace ? 0xc0000000 : 0x80000000;
            panic_if(result.hostPhysical !=
                     (base | (gpa & mask(12 + 9 * (leaf - 1)))) ||
                     result.permissions != (test == EptAdTest::Replace ? 5 : 7),
                     "EPT A/D returned a stale HPA or permissions");
        } else {
            panic_if(result.hostPhysical, "Failed EPT A/D exposed an HPA");
            if (test == EptAdTest::Revoke)
                panic_if(result.permissions != 5,
                         "EPT A/D retained stale rights after replacement");
        }
        if (test == EptAdTest::UpdateError)
            panic_if(result.entryAddress != descriptors.back() ||
                     result.level != leaf || result.entry,
                     "EPT A/D update error lost its address/level");
        for (unsigned i = 0; i < before.size(); ++i) {
            uint64_t expected = before[i];
            const bool last = i + 1 == before.size();
            if (test == EptAdTest::Replace || test == EptAdTest::Revoke) {
                if (i == 0)
                    expected &= ~2ULL;
                if (last)
                    expected = 0xc0000000ULL | 7 | (6 << 3) | Software |
                        (test == EptAdTest::Replace ? D : 0);
            }
            const bool unchanged = last && (test == EptAdTest::Misconfig ||
                test == EptAdTest::Absent ||
                test == EptAdTest::CancelQueued ||
                (test == EptAdTest::UpdateError && system->isAtomicMode()));
            if (!unchanged) {
                expected |= A;
                if (last && access == EptAccess::Write &&
                    test != EptAdTest::Denied && test != EptAdTest::Revoke)
                    expected |= D;
            }
            panic_if(descriptor(i) != expected,
                     "EPT A/D changed permissions, address or unrelated bits");
        }
        panic_if(test == EptAdTest::AlreadySet && progress->updates,
                 "EPT A/D issued a redundant update");
        panic_if((test == EptAdTest::Replace || test == EptAdTest::Revoke) &&
                 system->isTimingMode() &&
                 failedUpdates != 1, "EPT A/D replacement did not fail its compare");
        panic_if((test == EptAdTest::Retry || cancel) && injected != 1,
                 "EPT A/D did not exercise update retry/cancellation");
        inspectAttributes = {};
        inspectResponse = {};
        squash = {};
        const auto done = [this] {
            awaiting = false;
            schedule(nextEvent, curTick() + 1);
        };
        if (test == EptAdTest::Read && cacheDisabled) {
            // Reuse the authoritative descriptor to check a real masked
            // uncached write. Disabled bytes include its address and rights.
            const Addr address = descriptors.back();
            const Addr neighbor = address ^ 8;
            const auto expected =
                descriptor(descriptors.size() - 1) & ~(0xffULL << 56);
            const auto adjacent =
                system->physProxy.read<uint64_t>(neighbor, ByteOrder::little);
            auto req = std::make_shared<Request>(address, 8, Request::PHYSICAL,
                                                 requestor);
            paging::applyMemoryType(*req, paging::MemoryType::Uncacheable,
                                    true);
            std::vector<bool> enabled(8, false);
            enabled.back() = true;
            req->setByteEnable(enabled);
            auto *pkt = new Packet(req, MemCmd::WriteReq);
            pkt->allocate();
            pkt->setLE<uint64_t>(0);
            const auto check = [=, this](PacketPtr response, bool cancelled) {
                panic_if(cancelled || response->isError() ||
                             system->physProxy.read<uint64_t>(
                                 address, ByteOrder::little) != expected ||
                             system->physProxy.read<uint64_t>(
                                 neighbor, ByteOrder::little) != adjacent,
                         "Masked UC write changed disabled or adjacent bytes");
                delete response;
                done();
            };
            auto &physical = tlb->getWalker()->pagingPort();
            if (system->isTimingMode()) {
                physical.submit(pkt, [] { return true; }, check);
            } else {
                physical.sendAtomic(pkt);
                check(pkt, false);
            }
        } else {
            done();
        }
    };
    observedAtomicLatency = 0;
    awaiting = true;
    if (system->isTimingMode()) {
        ept::walkTiming(port, requestor, input, 48, Caps,
            [cancelled] { return !*cancelled; },
            [verify](const ept::WalkResult &result) { verify(result, 0); });
    } else {
        const auto result = ept::walkAtomic(port, requestor, input, 48, Caps);
        verify(result.result, result.latency);
    }
}

void
PagingTester::eptAdGuest(bool denied, bool functional, bool writing,
                         bool presetAD, bool cacheDisabled)
{
    if (!writing) {
        setupEpt(presetAD, true);
        CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
        cr0.cd = cacheDisabled;
        tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
    }
    // Preset guest A/D tests EPT write permission on an ordinary table read.
    // The additional cold write sets both guest and EPT A/D from clear bits.
    if (denied) {
        const auto raw = system->physProxy.read<uint64_t>(
            EptTableLeaves, ByteOrder::little);
        system->physProxy.write<uint64_t>(EptTableLeaves, raw & ~2ULL,
                                          ByteOrder::little);
    }
    const auto mode = writing || !presetAD ? BaseMMU::Write : BaseMMU::Read;
    std::vector<uint64_t> guestBefore;
    for (unsigned l = 0; l < 4; ++l)
        guestBefore.push_back(descriptor(l));
    std::array<uint8_t, 4 * 4096> eptBefore;
    system->physProxy.readBlob(EptRoot, eptBefore.data(), eptBefore.size());
    auto updates = std::make_shared<unsigned>(0);
    auto tableReads = std::make_shared<unsigned>(0);
    inspectAttributes = [=](PacketPtr pkt) {
        panic_if(pkt->req->memoryType() !=
                     (cacheDisabled ? Request::MemoryType::Uncacheable
                                    : Request::MemoryType::WriteBack),
                 "EPT A/D used the wrong captured table memory type");
        if (pkt->req->isAtomicReturn())
            ++*updates;
        else if (pkt->getAddr() >= HostTables &&
                 pkt->getAddr() < HostTables + 16384)
            ++*tableReads;
    };
    const Tick start = curTick();
    auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
        system->getRequestorId(this), Virtual, tc->contextId());
    const auto verify = [=, this](const Fault &fault, const RequestPtr &out,
                                  ThreadContext *context,
                                  BaseMMU::Mode returned) {
        panic_if(out != req || context != tc || returned != mode,
                 "EPT A/D guest callback lost its request/mode");
        if (denied) {
            const auto *exit = dynamic_cast<VmxExitFault *>(fault.get());
            panic_if(!exit || exit->info().reason != VmxExitReason::EptViolation ||
                     !exit->info().hasQualification ||
                     exit->info().qualification != (3 | (5 << 3) | 128) ||
                     !exit->info().hasGuestPhysicalAddress ||
                     exit->info().guestPhysicalAddress != Tables ||
                     !exit->info().hasGuestLinearAddress ||
                     exit->info().guestLinearAddress != Virtual || *tableReads,
                     "EPT A/D did not classify a guest table read as a write");
        } else {
            panic_if(fault || out->getPaddr() != HostData + (Virtual & 4095) ||
                     *tableReads != (writing ? 0 : 4),
                     "EPT A/D guest translation returned wrong HPA or rewalked");
        }
        for (unsigned l = 0; l < 4; ++l) {
            const auto expected = guestBefore[l] |
                (denied || functional ? 0 : 32) |
                (!denied && !functional && l == 3 && mode == BaseMMU::Write ?
                 64 : 0);
            panic_if(descriptor(l) != expected,
                     "EPT A/D lost guest A/D updates or changed unrelated bits");
            const auto leaf = system->physProxy.read<uint64_t>(
                EptTableLeaves + l * 8, ByteOrder::little);
            panic_if(bool(leaf & (1ULL << 9)) != (!denied && !functional),
                     "EPT A/D guest read did not dirty its EPT leaf");
        }
        const auto data = system->physProxy.read<uint64_t>(
            EptDataLeaf, ByteOrder::little);
        panic_if(bool(data & (1ULL << 9)) !=
                 (!denied && !functional && mode == BaseMMU::Write),
                 "EPT A/D dirtied final data on read or missed a warm write");
        if (functional) {
            std::array<uint8_t, 4 * 4096> after;
            system->physProxy.readBlob(EptRoot, after.data(), after.size());
            panic_if(after != eptBefore || *updates || curTick() != start,
                     "Functional guest translation modified EPT A/D or time");
        }
        panic_if(writing && *updates != 1,
                 "Warm guest write did not issue exactly one EPT dirty update");
        awaiting = false;
        if (!denied && !functional && !writing && presetAD) {
            schedule(new EventFunctionWrapper(
                         [=, this] {
                             eptAdGuest(false, false, true, true,
                                        cacheDisabled);
                         },
                         name() + ".ept_ad_warm", true),
                     curTick() + 1);
        } else {
            finishEpt();
        }
    };
    awaiting = true;
    if (functional) {
        verify(mmu->translateFunctional(req, tc, mode), req, tc, mode);
    } else if (system->isTimingMode()) {
        auto callback = std::make_unique<EptTranslation>(verify);
        auto *raw = callback.get();
        continuations.push_back(std::move(callback));
        mmu->translateTiming(req, tc, raw, mode);
    } else {
        verify(mmu->translateAtomic(req, tc, mode), req, tc, mode);
    }
}

void
PagingTester::eptMmuAccess(BaseMMU::Mode mode, bool functional, bool warm,
                          EptTest test, unsigned level, bool primed)
{
    if (!primed) {
        setupEpt(!functional && test != EptTest::TableUpdate &&
                               test != EptTest::Replace);
        if (test == EptTest::DirtyUpdate)
            descriptor(3, descriptor(3) & ~64ULL);
    }
    const bool priming = warm && !primed;
    const auto operationMode = priming && test == EptTest::DirtyUpdate ?
        BaseMMU::Read : mode;
    auto *cache = static_cast<TLB *>(mode == BaseMMU::Execute ?
                                    mmu->itb : mmu->dtb);
    const TlbContext tag{tc->contextId(), 0};
    panic_if(bool(cache->lookup(Virtual, false, tag)) != primed,
             "EPT MMU test did not take the intended TLB hit/miss path");
    if (primed && test != EptTest::DirtyUpdate) {
        // Leave the TLB intact but poison the guest descriptors. This also
        // catches an unexpected rewalk through the independent I-TLB port.
        for (unsigned l = 0; l < 4; ++l)
            descriptor(l, 0);
    }
    const auto changeRights = [this](Addr address, uint64_t rights) {
        auto raw = system->physProxy.read<uint64_t>(address, ByteOrder::little);
        system->physProxy.write<uint64_t>(address, (raw & ~7ULL) | rights,
                                          ByteOrder::little);
    };
    const auto current = priming ? EptTest::Mapping : test;
    const Addr tableLeaf = EptTableLeaves + 8 * level;
    switch (current) {
      case EptTest::DeniedRead: changeRights(EptDataLeaf, 4); break;
      case EptTest::DeniedWrite: changeRights(EptDataLeaf, 5); break;
      case EptTest::DeniedExecute: changeRights(EptDataLeaf, 3); break;
      case EptTest::TableRead: changeRights(tableLeaf, 4); break;
      case EptTest::TableUpdate: changeRights(tableLeaf, 5); break;
      case EptTest::DirtyUpdate: changeRights(tableLeaf, 5); break;
      case EptTest::Rmw: changeRights(EptDataLeaf, 5); break;
      case EptTest::DataMisconfig: changeRights(EptDataLeaf, 2); break;
      case EptTest::TableMisconfig: changeRights(tableLeaf, 2); break;
      case EptTest::GuestFault:
        descriptor(3, descriptor(3) & ~1ULL);
        changeRights(EptDataLeaf, 2); // Guest #PF precedes the final EPT lookup.
        break;
      case EptTest::Replace:
        replacement = (Physical + 4096) | 7 | 64 | 512;
        injection = ReplaceLeaf;
        break;
      case EptTest::Remap:
        system->physProxy.write<uint64_t>(EptDataLeaf,
            OtherHostData | 7 | (6ULL << 3) | 128, ByteOrder::little);
        break;
      case EptTest::InheritedWrite:
        changeRights(EptRoot, 5);
        changeRights(EptDataLeaf, 3);
        break;
      default: break;
    }
    const bool tableAccess = current == EptTest::TableRead ||
        current == EptTest::TableUpdate || current == EptTest::DirtyUpdate ||
        current == EptTest::TableMisconfig;
    const bool misconfig = current == EptTest::DataMisconfig ||
        current == EptTest::TableMisconfig;
    const bool success = current == EptTest::Mapping ||
        current == EptTest::Replace || current == EptTest::Remap;
    const Addr expectedHpa = (current == EptTest::Remap ? OtherHostData :
        HostData + (current == EptTest::Replace ? 4096 : 0)) + (Virtual & 4095);
    std::vector<uint64_t> before;
    for (unsigned l = 0; l < 4; ++l)
        before.push_back(descriptor(l));
    auto eptBefore = std::make_shared<std::array<uint8_t, 4 * 4096>>();
    system->physProxy.readBlob(EptRoot, eptBefore->data(), eptBefore->size());
    auto tableRequests = std::make_shared<unsigned>(0);
    inspectAttributes = [=](PacketPtr pkt) {
        if (pkt->getAddr() >= HostTables && pkt->getAddr() < HostTables + 16384)
            ++*tableRequests;
    };
    const Tick start = curTick();
    auto flags = accessFlags;
    if (current == EptTest::Rmw)
        flags.set(Request::READ_MODIFY_WRITE);
    if (mode == BaseMMU::Execute)
        flags.set(Request::INST_FETCH);
    auto req = std::make_shared<Request>(Virtual, 1, flags,
        system->getRequestorId(this), Virtual, tc->contextId());
    const auto retired = [=, this] {
        std::array<uint8_t, 4 * 4096> eptAfter;
        system->physProxy.readBlob(EptRoot, eptAfter.data(), eptAfter.size());
        panic_if(eptAfter != *eptBefore, "Guest paging modified EPT entries");
        panic_if(primed && current != EptTest::DirtyUpdate && *tableRequests,
                 "EPT MMU TLB hit repeated a guest table access");
        for (unsigned l = 0; l < 4; ++l) {
            const Addr gpa = Tables + l * 4096 + (descriptors[l] & 4095);
            panic_if(system->physProxy.read<uint64_t>(gpa, ByteOrder::little),
                     "EPT MMU modified a GPA instead of its backing HPA");
            if (functional)
                panic_if(descriptor(l) != before[l],
                         "Functional EPT MMU changed guest A/D bits");
        }
        panic_if(functional && curTick() != start,
                 "Functional EPT MMU advanced simulated time");
        if (current == EptTest::Replace) {
            panic_if(injected != 1 || descriptor(3) != (replacement | 32) ||
                     (system->isTimingMode() && failedUpdates != 1),
                     "Nested A/D retry lost the new mapping, D or software bits");
        }
        if (current == EptTest::TableUpdate) {
            for (unsigned l = 0; l < 4; ++l)
                panic_if(bool(descriptor(l) & 32) != (l < level),
                         "Denied nested A/D update modified the wrong level");
        }
        if (current == EptTest::DirtyUpdate)
            panic_if(descriptor(3) & 64,
                     "EPT-denied guest dirty update set D");
        awaiting = false;
        if (priming) {
            schedule(new EventFunctionWrapper([=, this] {
                eptMmuAccess(mode, functional, warm, test, level, true);
            }, name() + ".ept_warm", true), curTick() + 1);
        } else {
            finishEpt();
        }
    };
    const auto translated = [=, this](const Fault &fault, const RequestPtr &out,
                                       ThreadContext *context,
                                       BaseMMU::Mode returnedMode) {
        panic_if(out != req || context != tc || returnedMode != operationMode,
                 "EPT MMU changed the request, thread or original mode");
        if (!success) {
            panic_if((functional || tableAccess ||
                      current == EptTest::GuestFault) &&
                     cache->lookup(Virtual, false, tag),
                     "Failed guest/functional walk filled the TLB");
            if (current == EptTest::GuestFault) {
                const auto *page = dynamic_cast<PageFault *>(fault.get());
                panic_if(!page || page->describe() != PageFault(Virtual,
                    false, mode, false, false).describe(),
                    "Nested paging lost guest page-fault priority");
            } else {
                const auto *exit = dynamic_cast<VmxExitFault *>(fault.get());
                panic_if(!exit, "EPT MMU failure was not a VM-exit fault");
                const auto &info = exit->info();
                const Addr gpa = tableAccess ? Tables + level * 4096 +
                    (descriptors[level] & 4095) : Physical + (Virtual & 4095);
                const uint64_t permissions = current == EptTest::DeniedRead ||
                    current == EptTest::TableRead ? 4 :
                    current == EptTest::DeniedExecute ? 3 :
                    current == EptTest::InheritedWrite ? 1 : 5;
                const uint64_t access = tableAccess ?
                    (current == EptTest::TableUpdate ||
                     current == EptTest::DirtyUpdate ? 2 : 1) :
                    current == EptTest::Rmw ? 2 :
                    mode == BaseMMU::Read ? 1 : mode == BaseMMU::Write ? 2 : 4;
                const uint64_t qualification = access | (permissions << 3) |
                    128 | (tableAccess ? 0 : 256);
                panic_if(info.reason != (misconfig ?
                         VmxExitReason::EptMisconfiguration :
                         VmxExitReason::EptViolation) || info.vmEntryFailure ||
                         !info.hasGuestPhysicalAddress ||
                         info.guestPhysicalAddress != gpa ||
                         info.hasQualification != !misconfig ||
                         info.hasGuestLinearAddress != !misconfig ||
                         info.hasInstructionLength || info.hasInstructionInfo ||
                         info.hasInterruptionInfo || info.hasInterruptionErrorCode ||
                         (!misconfig && (info.qualification != qualification ||
                          info.guestLinearAddress != Virtual)),
                         "EPT MMU reported wrong exit metadata");
            }
            retired();
            return;
        }
        panic_if(fault || out->getPaddr() != expectedHpa,
                 "EPT MMU returned wrong final HPA or fault");
        if (!functional) {
            const auto *entry = cache->lookup(Virtual, false, tag);
            panic_if(!entry || entry->paddr != Physical +
                     (current == EptTest::Replace ? 4096 : 0),
                     "EPT MMU cached an HPA instead of a guest-physical page");
        }
        auto *packet = operationMode == BaseMMU::Write ?
            Packet::createWrite(out) : Packet::createRead(out);
        packet->allocate();
        if (operationMode == BaseMMU::Write)
            packet->setLE<uint8_t>(0xa5);
        const auto dataDone = [=, this](PacketPtr response, bool cancelled) {
            panic_if(cancelled || response->isError() ||
                     (operationMode != BaseMMU::Write &&
                      response->getLE<uint8_t>() !=
                        (current == EptTest::Remap ? 0xeb : 0x90)),
                     "EPT MMU accessed incorrect instruction/data bytes");
            delete response;
            panic_if(system->physProxy.read<uint8_t>(Physical +
                     (Virtual & 4095)) != 0xcc ||
                     (operationMode == BaseMMU::Write &&
                      system->physProxy.read<uint8_t>(expectedHpa) != 0xa5),
                     "EPT MMU final write reached the wrong physical page");
            retired();
        };
        auto &port = tlb->getWalker()->pagingPort();
        if (functional) {
            port.sendFunctional(packet);
            dataDone(packet, false);
        } else if (system->isTimingMode()) {
            port.submit(packet, [] { return true; }, dataDone);
        } else {
            port.sendAtomic(packet);
            dataDone(packet, false);
        }
    };
    awaiting = true;
    if (functional) {
        translated(mmu->translateFunctional(req, tc, operationMode), req, tc,
                   operationMode);
    } else if (system->isTimingMode()) {
        auto callback = std::make_unique<EptTranslation>(translated);
        auto *raw = callback.get();
        continuations.push_back(std::move(callback));
        mmu->translateTiming(req, tc, raw, operationMode);
    } else {
        translated(mmu->translateAtomic(req, tc, operationMode), req, tc,
                   operationMode);
    }
}

void
PagingTester::eptMmuTiming(unsigned point, Injection inject, bool warm,
                          bool primed)
{
    if (!primed)
        setupEpt(point == 2);
    // Points select nested table-read lookup, A/D-write lookup, final lookup,
    // the conditional update packet, or cancellation before admission.
    if (warm && !primed) {
        auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
            system->getRequestorId(this), Virtual, tc->contextId());
        auto callback = std::make_unique<EptTranslation>(
            [=, this](const Fault &fault, const RequestPtr &, ThreadContext *,
                       BaseMMU::Mode) {
                panic_if(fault, "EPT lifecycle warm-up failed");
                schedule(new EventFunctionWrapper([=, this] {
                    eptMmuTiming(point, inject, warm, true);
                }, name() + ".ept_lifecycle_warm", true), curTick() + 1);
            });
        auto *raw = callback.get();
        continuations.push_back(std::move(callback));
        awaiting = true;
        mmu->translateTiming(req, tc, raw, BaseMMU::Read);
        return;
    }
    const auto mode = point == 2 ? BaseMMU::Read : BaseMMU::Write;
    auto cancelled = std::make_shared<bool>(point == 4);
    squash = [cancelled] { *cancelled = true; };
    auto roots = std::make_shared<unsigned>(0);
    auto armed = std::make_shared<bool>(false);
    const unsigned targetRoot = point == 1 ? 2 : point == 2 && !warm ? 5 : 1;
    inspectAttributes = [=, this](PacketPtr pkt) {
        if (pkt->getAddr() == EptRoot)
            ++*roots;
        const bool target = point == 3 ? pkt->req->isAtomicReturn() :
            pkt->getAddr() == EptRoot && *roots == targetRoot;
        if (*armed || !target)
            return;
        *armed = true;
        injection = point != 3 ? inject :
            inject == RetryRead ? RetryUpdate :
            inject == SquashRetryRead ? SquashRetryUpdate :
            inject == SquashRead ? SquashUpdate : InvalidateUpdate;
        if (inject == RetryRead) {
            // Every subsequent read must use the acceptance-time snapshot,
            // including the final stage after guest paging completes.
            static_cast<ISA *>(tc->getIsaPtr())->vmxState().activeEptConfig.reset();
            tc->setMiscRegNoEffect(misc_reg::VmxEptVpidCap, 0);
        }
    };
    inspectResponse = [=](PacketPtr pkt) {
        if (*cancelled && !pkt->req->isAtomicReturn())
            pkt->setBadAddress(); // Cancellation must win over memory errors.
    };
    auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
        system->getRequestorId(this), Virtual, tc->contextId());
    auto callback = std::make_unique<EptTranslation>(
        [=, this](const Fault &fault, const RequestPtr &out,
                   ThreadContext *context, BaseMMU::Mode returnedMode) {
            panic_if(out != req || context != tc || returnedMode != mode ||
                     injected != (point == 4 ? 0 : 1),
                     "Nested EPT lifecycle callback or injection failed");
            const bool retry = inject == RetryRead;
            panic_if(retry ? bool(fault) || out->getPaddr() !=
                         HostData + (Virtual & 4095) :
                         !dynamic_cast<ReExec *>(fault.get()),
                     "Nested EPT lifecycle returned wrong HPA/fault");
            if (!retry) {
                panic_if(!warm && tlb->lookup(Virtual, false,
                    {tc->contextId(), 0}) && point != 2,
                    "Cancelled guest walk published a TLB entry");
                for (unsigned l = 0; l < 4; ++l) {
                    const bool accessed = point == 2 ||
                        (point == 3 && (inject == SquashRead ||
                                       inject == InvalidateRead) && l == 0);
                    panic_if(bool(descriptor(l) & 32) != accessed ||
                             bool(descriptor(l) & 64) != (point == 2 && l == 3),
                             "Nested EPT cancellation changed unexpected A/D bits");
                }
            }
            finishEpt();
        }, cancelled);
    auto *raw = callback.get();
    continuations.push_back(std::move(callback));
    awaiting = true;
    mmu->translateTiming(req, tc, raw, mode);
    panic_if(mmu->drain() != DrainState::Draining,
             "Nested EPT lost MMU ownership before retirement");
}

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
            panic_if(result.hostPhysical ||
                     result.permissions !=
                         (expected == WalkStatus::Violation ? permissions : 0) ||
                     result.pageShift || result.memoryType || result.ignorePat,
                     "EPT %s: failure exposed a mapping or wrong rights", label);
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
    check("denied write", WalkStatus::Violation, 4, EptAccess::Write, 1);
    check("denied execute", WalkStatus::Violation, 4, EptAccess::Execute, 1);
    descriptor(3, (descriptor(3) & ~7ULL) | 4); // X-only leaf
    check("empty intersection", WalkStatus::Violation, 4, EptAccess::Execute, 0);
    for (unsigned i = 0; i < 4; ++i)
        descriptor(i, (descriptor(i) & ~7ULL) | 4);
    check("execute-only", WalkStatus::Mapping, 4, EptAccess::Execute, 4);
    check("denied read", WalkStatus::Violation, 4, EptAccess::Read, 4);
    capability &= ~1ULL;
    check("unsupported execute-only", WalkStatus::Misconfiguration, 1);

    for (unsigned levels : {4, 5}) {
        for (unsigned stop = 0; stop < levels; ++stop) {
            setup(levels);
            // Absent entries win over otherwise-reserved contents.
            descriptor(stop, (1ULL << 48) | 128 | (7 << 3));
            check("absent", WalkStatus::Violation, stop + 1,
                  EptAccess::Read, 0);
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
        check("GPA outside walk width",
              levels == 4 ? WalkStatus::Violation : WalkStatus::InvalidInput,
              0, EptAccess::Read, 0);
    }
    for (unsigned bit = 48; bit < 52; ++bit) {
        for (bool ad : {false, true}) {
            for (auto access :
                 {EptAccess::Read, EptAccess::Write, EptAccess::Execute}) {
                setup();
                physicalBits = 52;
                config = *ept::decodeEptp(config.eptp | (ad ? 64 : 0),
                                          capability, physicalBits);
                gpa |= 1ULL << bit;
                check("four-level GPA overflow", WalkStatus::Violation, 0,
                      access, 0);
            }
        }
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
        gpa |= 1ULL << 48; // Invalid configuration must win over overflow.
        check("inconsistent captured config", WalkStatus::InvalidInput, 0);
    }
    for (unsigned bit : {6, 14}) {
        setup();
        capability &= ~(1ULL << bit);
        check("unsupported EPTP capability", WalkStatus::InvalidInput, 0);
    }
    setup();
    config = *ept::decodeEptp(config.eptp | 64, capability, physicalBits);
    capability &= ~(1ULL << 21);
    check("unsupported A/D capability", WalkStatus::InvalidInput, 0);
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
