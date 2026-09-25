/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/paging_tester.hh"

#include <array>

#include "arch/x86/faults.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/ldstflags.hh"
#include "arch/x86/mmu.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/paging.hh"
#include "arch/x86/x86_traits.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/tlb.hh"
#include "arch/x86/types.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "mem/port_proxy.hh"
#include "mem/packet_access.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5::X86ISA
{
namespace
{
class TestContinuation : public BaseMMU::Translation
{
  public:
    using Callback = std::function<void(const Fault &, const RequestPtr &,
                                       ThreadContext *, BaseMMU::Mode)>;
    explicit TestContinuation(Callback action,
                              std::shared_ptr<bool> cancelled = nullptr)
        : callback(std::move(action)), cancelled(std::move(cancelled)) {}
    bool squashed() const override { return cancelled && *cancelled; }
    void markDelayed() override {}
    void finish(const Fault &fault, const RequestPtr &req,
                ThreadContext *tc, BaseMMU::Mode mode) override
    { callback(fault, req, tc, mode); }
  private:
    Callback callback;
    std::shared_ptr<bool> cancelled;
};
} // anonymous namespace

PagingTester::PagingTester(const X86PagingTesterParams &p)
    : SimObject(p), system(p.system), transportCases(p.transport_cases),
      upstream(*this), downstream(*this),
      retryEvent([this] { upstream.sendRetryReq(); }, name() + ".retry"),
      nextEvent([this] { next(); }, name() + ".next")
{
    system->getRequestorId(this);
}

PagingTester::Upstream::Upstream(PagingTester &tester)
    : ResponsePort(tester.name() + ".cpu_side"), owner(tester)
{}

PagingTester::Downstream::Downstream(PagingTester &tester)
    : RequestPort(tester.name() + ".mem_side"), owner(tester)
{}

Port &
PagingTester::getPort(const std::string &name, PortID index)
{
    if (name == "cpu_side")
        return upstream;
    if (name == "mem_side")
        return downstream;
    return SimObject::getPort(name, index);
}

AddrRangeList
PagingTester::Upstream::getAddrRanges() const
{
    return owner.downstream.getAddrRanges();
}

Tick
PagingTester::Upstream::recvAtomic(PacketPtr pkt)
{
    if (owner.inspectAttributes)
        owner.inspectAttributes(pkt);
    owner.replace(pkt);
    return owner.downstream.sendAtomic(pkt);
}

void
PagingTester::Upstream::recvFunctional(PacketPtr pkt)
{
    if (owner.inspectAttributes)
        owner.inspectAttributes(pkt);
    owner.downstream.sendFunctional(pkt);
}

bool
PagingTester::Upstream::recvTimingReq(PacketPtr pkt)
{
    return owner.sendTiming(pkt);
}

void
PagingTester::Upstream::recvRespRetry()
{
    owner.downstream.sendRetryResp();
}

bool
PagingTester::Downstream::recvTimingResp(PacketPtr pkt)
{
    panic_if(!owner.pendingPackets, "Unowned fixture response");
    --owner.pendingPackets;
    if (pkt->req->isAtomicReturn()) {
        const uint64_t previous = pkt->getSize() == 8 ?
            pkt->getLE<uint64_t>() : pkt->getLE<uint32_t>();
        if (previous != pkt->req->getExtraData())
            ++owner.failedUpdates;
    }
    return owner.upstream.sendTimingResp(pkt);
}

void
PagingTester::Downstream::recvReqRetry()
{
    owner.upstream.sendRetryReq();
}

void
PagingTester::Downstream::recvRangeChange()
{
    owner.upstream.sendRangeChange();
}

void
PagingTester::replace(PacketPtr packet)
{
    if (injection == ReplaceLeaf && packet->req->isAtomicReturn() &&
        packet->getAddr() == descriptors.back()) {
        injection = None;
        ++injected;
        descriptor(descriptors.size() - 1, replacement);
    }
}

bool
PagingTester::sendTiming(PacketPtr packet)
{
    if (inspectAttributes)
        inspectAttributes(packet);
    replace(packet);
    const bool update = packet->req->isAtomicReturn();
    const bool read = !update && packet->isRead();
    if (update && injection == PeerUpdate &&
        packet->getAddr() == descriptors.back()) {
        injection = None;
        ++injected;
        // Hold the first walker's observed leaf until a second CPU's walker
        // has updated it through its own coherent memory endpoint.
        startPeer();
        return false;
    }
    if (read && injection == FunctionalDuringRead) {
        injection = None;
        ++injected;
        auto nested = std::make_shared<Request>(*request);
        const auto fault = mmu->translateFunctional(nested, tc, BaseMMU::Write);
        panic_if(fault || nested->getPaddr() != Physical + (Virtual & 4095),
                 "Functional translation failed during a timing walk");
        for (unsigned l = 0; l < descriptors.size(); ++l)
            panic_if(descriptor(l) & 96, "Functional walk changed A/D bits");
        panic_if(tlb->lookup(Virtual, false), "Functional walk filled the TLB");
        panic_if(mmu->drain() != DrainState::Draining,
                 "Functional access lost pending MMU continuation ownership");
    }
    if ((read && injection == SquashRetryRead) ||
        (update && injection == SquashRetryUpdate)) {
        squash();
        injection = None;
        ++injected;
        schedule(retryEvent, curTick() + 10);
        return false;
    }
    const bool retry = (read && (injection == RetryRead ||
                                injection == InvalidateRetryRead)) ||
        (update && (injection == RetryUpdate ||
                    injection == InvalidateRetryUpdate));
    if (retry) {
        if (injection == InvalidateRetryRead ||
            injection == InvalidateRetryUpdate)
            mmu->flushAll();
        injection = None;
        ++injected;
        schedule(retryEvent, curTick() + 10);
        return false;
    }
    const bool invalidate = (read && injection == InvalidateRead) ||
                            (update && injection == InvalidateUpdate);
    const bool cancel = (read && injection == SquashRead) ||
                        (update && injection == SquashUpdate);
    if (!downstream.sendTimingReq(packet))
        return false;
    ++pendingPackets;
    if (cancel) {
        squash();
        injection = None;
        ++injected;
    }
    if (invalidate) {
        mmu->flushAll();
        injection = None;
        ++injected;
    }
    return true;
}

void
PagingTester::startup()
{
    tc = system->threads[0];
    for (auto *thread : system->threads)
        thread->suspend();
    mmu = static_cast<MMU *>(tc->getMMUPtr());
    tlb = static_cast<TLB *>(mmu->dtb);
    buildCases();
    schedule(nextEvent, curTick());
}

uint64_t
PagingTester::descriptor(unsigned level) const
{
    if (descriptorSize == 8)
        return system->physProxy.read<uint64_t>(descriptors.at(level),
                                                ByteOrder::little);
    return system->physProxy.read<uint32_t>(descriptors.at(level),
                                            ByteOrder::little);
}

void
PagingTester::descriptor(unsigned level, uint64_t value)
{
    if (descriptorSize == 8)
        system->physProxy.write<uint64_t>(descriptors.at(level), value,
                                         ByteOrder::little);
    else
        system->physProxy.write<uint32_t>(descriptors.at(level), value,
                                         ByteOrder::little);
}

void
PagingTester::setup(Format format, bool large, bool user)
{
    mmu->flushAll();
    std::array<uint8_t, 4 * 4096> zero = {};
    system->physProxy.writeBlob(Tables, zero.data(), zero.size());
    descriptors.clear();
    inspectAttributes = {};
    injection = None;
    injected = 0;
    failedUpdates = 0;
    accessFlags = segment_idx::Ds | (3 << AddrSizeFlagShift);
    descriptorSize = format == Long || format == Pae ? 8 : 4;
    Efer efer = 0;
    efer.lme = efer.lma = format == Long;
    efer.nxe = 1;
    CR0 cr0 = 0;
    cr0.pe = cr0.pg = cr0.wp = cr0.et = 1;
    CR4 cr4 = 0;
    cr4.pae = format == Long || format == Pae;
    cr4.pse = format == Pse;
    cr4.pge = 1;
    tc->setMiscRegNoEffect(misc_reg::Efer, efer);
    tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
    tc->setMiscRegNoEffect(misc_reg::Cr4, cr4);
    tc->setMiscRegNoEffect(misc_reg::Cr3, Tables);
    HandyM5Reg handy = 0;
    handy.mode = format == Long ? LongMode : LegacyMode;
    handy.submode = format == Long ? SixtyFourBitMode : ProtectedMode;
    handy.cpl = user ? 3 : 0;
    handy.prot = handy.paging = 1;
    tc->setMiscRegNoEffect(misc_reg::M5Reg, handy);
    SegAttr attr = 0;
    attr.present = attr.readable = attr.writable = 1;
    for (int seg : {segment_idx::Ds, segment_idx::Ss, segment_idx::Cs}) {
        tc->setMiscRegNoEffect(misc_reg::segAttr(seg), attr);
        tc->setMiscRegNoEffect(misc_reg::segBase(seg), 0);
        tc->setMiscRegNoEffect(misc_reg::segLimit(seg), 0xffffffff);
    }
    std::vector<unsigned> shifts;
    if (format == Long)
        shifts = large ? std::vector<unsigned>{39, 30, 21} :
                         std::vector<unsigned>{39, 30, 21, 12};
    else if (format == Pae)
        shifts = large ? std::vector<unsigned>{30, 21} :
                         std::vector<unsigned>{30, 21, 12};
    else
        shifts = large ? std::vector<unsigned>{22} :
                         std::vector<unsigned>{22, 12};
    for (unsigned level = 0; level < shifts.size(); ++level) {
        const Addr table = Tables + level * 4096;
        const Addr slot = (Virtual >> shifts[level]) &
            (descriptorSize == 8 ? 511 : 1023);
        descriptors.push_back(table + slot * descriptorSize);
        const bool leaf = level + 1 == shifts.size();
        uint64_t value = (leaf ? Physical : table + 4096) | 7;
        if (format == Pae && level == 0)
            value &= ~6ULL; // legacy PDPTE U/S and R/W are reserved
        if (large && leaf)
            value |= 128;
        descriptor(level, value);
    }
    if (format == Pae) {
        std::array<RegVal, 4> values{};
        values[Virtual >> 30] = descriptor(0);
        panic_if(static_cast<ISA *>(tc->getIsaPtr())->loadPaePdpte(values),
                 "Fixture constructed an invalid PDPTE");
    }
}

void
PagingTester::translate(BaseMMU::Mode mode, Addr address, Addr physical,
        const Fault &fault, bool functional, std::function<void()> check)
{
    panic_if(awaiting, "Overlapping fixture requests");
    expectedMode = mode;
    expectedAddress = physical;
    expectedFault = fault;
    after = std::move(check);
    request = std::make_shared<Request>(address, 1, accessFlags,
        system->getRequestorId(this), address, tc->contextId());
    awaiting = true;
    if (functional) {
        auto fault = mmu->translateFunctional(request, tc, mode);
        finish(fault, request, tc, mode);
    } else if (system->isTimingMode()) {
        mmu->translateTiming(request, tc, this, mode);
    } else {
        auto fault = mmu->translateAtomic(request, tc, mode);
        finish(fault, request, tc, mode);
    }
}

void
PagingTester::finish(const Fault &fault, const RequestPtr &req,
                    ThreadContext *context, BaseMMU::Mode mode)
{
    panic_if(!awaiting || req != request || context != tc ||
             mode != expectedMode, "Fixture callback contract failed");
    awaiting = false;
    const auto describe = [](const Fault &fault) -> std::string {
        if (auto *page = dynamic_cast<PageFault *>(fault.get()))
            return page->describe();
        return fault ? fault->name() : "success";
    };
    const auto actual = describe(fault);
    const auto expected = describe(expectedFault);
    panic_if(actual != expected, "Paging fixture case %u: %s, expected %s",
             index, actual, expected);
    panic_if(!fault && req->getPaddr() != expectedAddress,
             "Paging fixture case %u: address %#x, expected %#x",
             index, req->getPaddr(), expectedAddress);
    if (after)
        after();
    schedule(nextEvent, curTick() + 1);
}

void
PagingTester::next()
{
    panic_if(awaiting, "Advanced the fixture with outstanding translations");
    panic_if(pendingPackets || tlb->getWalker()->drain() != DrainState::Drained,
             "Fixture advanced before walker retirement");
    for (auto *thread : system->threads) {
        auto *owner = static_cast<MMU *>(thread->getMMUPtr());
        panic_if(owner->getDataWalker()->drain() != DrainState::Drained ||
                 static_cast<TLB *>(owner->itb)->getWalker()->drain() !=
                     DrainState::Drained, "Peer I/D walker did not retire");
        panic_if(owner->drain() != DrainState::Drained,
                 "MMU continuation survived walker retirement");
    }
    continuations.clear();
    if (index == cases.size()) {
        inform("Paging fixture: %u cases passed", index);
        exitSimLoop("paging fixture passed");
        return;
    }
    cases[index++]();
}

void
PagingTester::queuePair(bool invalidate)
{
    setup(Long);
    CR4 cr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
    cr4.pcide = 1;
    tc->setMiscRegNoEffect(misc_reg::Cr4, cr4);
    tc->setMiscRegNoEffect(misc_reg::Cr3, Tables | 9);
    struct Pair
    {
        std::array<RequestPtr, 2> requests;
        std::array<bool, 2> seen = {};
        unsigned completed = 0;
    };
    auto pair = std::make_shared<Pair>();
    awaiting = true;
    for (unsigned i = 0; i < 2; ++i) {
        pair->requests[i] = std::make_shared<Request>(Virtual, 1, accessFlags,
            system->getRequestorId(this), Virtual, tc->contextId());
        auto callback = std::make_unique<TestContinuation>(
            [=, this](const Fault &fault, const RequestPtr &req,
                      ThreadContext *context, BaseMMU::Mode mode) {
                panic_if(pair->seen[i] || req != pair->requests[i] ||
                         context != tc || mode != BaseMMU::Read,
                         "Queued translation completion contract failed");
                pair->seen[i] = true;
                if (invalidate) {
                    panic_if(!dynamic_cast<ReExec *>(fault.get()),
                             "Obsolete queued walk did not restart");
                } else {
                    panic_if(fault || req->getPaddr() != Physical + (Virtual & 4095),
                             "Queued context snapshot returned a wrong result");
                }
                if (++pair->completed == 2) {
                    awaiting = false;
                    panic_if(tlb->lookup(Virtual, false, {tc->contextId(), 17}),
                             "Old walk filled under the new PCID");
                    panic_if(bool(tlb->lookup(Virtual, false, {tc->contextId(), 9})) == invalidate,
                             "Queued walk publication disagrees with generation");
                    schedule(nextEvent, curTick() + 1);
                }
            });
        mmu->translateTiming(pair->requests[i], tc, callback.get(), BaseMMU::Read);
        continuations.push_back(std::move(callback));
    }
    // Deliberately bypass architectural CR3 invalidation to isolate the
    // acceptance-time snapshot. The other case tests explicit invalidation.
    tc->setMiscRegNoEffect(misc_reg::Cr3, Tables | 17);
    if (invalidate)
        mmu->flushAll();
}

void
PagingTester::queuePae()
{
    setup(Pae);
    // The middle request selects an absent retained PDPTE. Its queued
    // completion must not stall the following request or complete twice.
    struct Batch
    {
        std::array<RequestPtr, 3> requests;
        std::array<bool, 3> seen = {};
        unsigned completed = 0;
    };
    auto batch = std::make_shared<Batch>();
    awaiting = true;
    for (unsigned i = 0; i < 3; ++i) {
        Addr address = i == 1 ? Virtual ^ (1ULL << 30) : Virtual;
        batch->requests[i] = std::make_shared<Request>(address, 1, accessFlags,
            system->getRequestorId(this), address, tc->contextId());
        auto callback = std::make_unique<TestContinuation>(
            [=, this](const Fault &fault, const RequestPtr &req,
                      ThreadContext *context, BaseMMU::Mode mode) {
                panic_if(batch->seen[i] || req != batch->requests[i] ||
                         context != tc || mode != BaseMMU::Read,
                         "Queued PAE callback contract failed");
                batch->seen[i] = true;
                if (i == 1) {
                    auto *page = dynamic_cast<PageFault *>(fault.get());
                    auto expected = PageFault(address, false, BaseMMU::Read,
                                               false, false);
                    panic_if(!page || page->describe() != expected.describe(),
                             "Queued absent PDPTE did not return #PF");
                } else {
                    panic_if(fault || req->getPaddr() != Physical + (Virtual & 4095),
                             "Queued PAE translation failed");
                }
                if (++batch->completed == 3) {
                    awaiting = false;
                    schedule(nextEvent, curTick() + 1);
                }
            });
        auto *continuation = callback.get();
        continuations.push_back(std::move(callback));
        mmu->translateTiming(batch->requests[i], tc, continuation, BaseMMU::Read);
    }
}

void
PagingTester::reentrantRetry()
{
    setup(Long);
    awaiting = true;
    auto count = std::make_shared<unsigned>(0);
    auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
        system->getRequestorId(this), Virtual, tc->contextId());
    auto second = std::make_unique<TestContinuation>(
        [=, this](const Fault &fault, const RequestPtr &result,
                  ThreadContext *context, BaseMMU::Mode mode) {
            panic_if(++*count != 2 || result != req || context != tc ||
                     mode != BaseMMU::Read || !dynamic_cast<ReExec *>(fault.get()),
                     "Reentrant translation completion contract failed");
            panic_if(pendingPackets,
                     "Cancelled reentrant walk completed before its response");
            awaiting = false;
            schedule(nextEvent, curTick() + 1);
        });
    auto *resume = second.get();
    continuations.push_back(std::move(second));
    auto first = std::make_unique<TestContinuation>(
        [=, this](const Fault &fault, const RequestPtr &result,
                  ThreadContext *context, BaseMMU::Mode mode) {
            panic_if(++*count != 1 || result != req || context != tc ||
                     mode != BaseMMU::Read || !dynamic_cast<ReExec *>(fault.get()),
                     "Rejected translation completion contract failed");
            injection = InvalidateRead;
            mmu->translateTiming(req, tc, resume, BaseMMU::Read);
            panic_if(tlb->getWalker()->drain() != DrainState::Draining,
                     "Reentrant walk lost drain ownership");
            panic_if(mmu->drain() != DrainState::Draining,
                     "Reentrant MMU continuation lost drain ownership");
        });
    auto *begin = first.get();
    continuations.push_back(std::move(first));
    injection = InvalidateRetryRead;
    mmu->translateTiming(req, tc, begin, BaseMMU::Read);
}

void
PagingTester::concurrentUpdate(Format format, bool large)
{
    setup(format, large);
    auto *peer = system->threads[1];
    static_cast<ISA *>(peer->getIsaPtr())->copyRegsFrom(tc);
    auto *peerMmu = static_cast<MMU *>(peer->getMMUPtr());
    peerMmu->flushAll();
    const unsigned leaf = descriptors.size() - 1;
    const auto original = descriptor(leaf) | (1ULL << 9);
    descriptor(leaf, original);
    auto completed = std::make_shared<bool>(false);
    auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
        system->getRequestorId(this), Virtual, peer->contextId());
    const Addr physical = Physical + (Virtual & ((1ULL <<
        (large ? (format == Pse ? 22 : 21) : 12)) - 1));
    auto callback = std::make_unique<TestContinuation>(
        [=, this](const Fault &fault, const RequestPtr &result,
                  ThreadContext *context, BaseMMU::Mode mode) {
            panic_if(*completed || fault || result != req || context != peer ||
                     mode != BaseMMU::Write || result->getPaddr() != physical,
                     "Peer walker write failed");
            *completed = true;
            schedule(retryEvent, curTick() + 10);
        });
    auto *continuation = callback.get();
    continuations.push_back(std::move(callback));
    startPeer = [=] {
        peerMmu->translateTiming(req, peer, continuation, BaseMMU::Write);
    };
    injection = PeerUpdate;
    translate(BaseMMU::Read, Virtual, physical, NoFault, false, [=, this] {
        panic_if(!*completed || injected != 1 || failedUpdates != 1,
                 "Concurrent descriptor comparison was not exercised");
        panic_if(descriptor(leaf) != (original | 96),
                 "Concurrent walkers lost dirty or unrelated descriptor bits");
        for (unsigned l = format == Pae ? 1 : 0; l < descriptors.size(); ++l)
            panic_if(!(descriptor(l) & 32), "Concurrent walk lost accessed bit");
    });
}

void
PagingTester::sharedTransport()
{
    setup(Long);
    auto &port = tlb->getWalker()->pagingPort();
    awaiting = true;
    struct Batch { unsigned done = 0; std::array<bool, 4> seen = {}; };
    auto batch = std::make_shared<Batch>();
    auto done = [=, this](unsigned slot) {
        panic_if(batch->seen[slot], "Transport callback completed twice");
        batch->seen[slot] = true;
        panic_if(tlb->getWalker()->drain() != DrainState::Draining,
                 "Transport callback lost drain ownership");
        if (++batch->done == 4) {
            awaiting = false;
            schedule(nextEvent, curTick() + 1);
        }
    };
    auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
        system->getRequestorId(this), Virtual, tc->contextId());
    auto callback = std::make_unique<TestContinuation>(
        [=](const Fault &fault, const RequestPtr &result,
            ThreadContext *, BaseMMU::Mode) {
            panic_if(fault || result != req ||
                     result->getPaddr() != Physical + (Virtual & 4095),
                     "Parent walk failed with shared transport");
            done(0);
        });
    auto *continuation = callback.get();
    continuations.push_back(std::move(callback));
    injection = RetryRead;
    mmu->translateTiming(req, tc, continuation, BaseMMU::Read);

    // This direct client bypasses the guest admission queue. Its cancelled
    // SwapReq must never update memory, even after port backpressure clears.
    const Addr target = Tables + 64;
    system->physProxy.write<uint64_t>(target, 0x123, ByteOrder::little);
    auto update = std::make_shared<Request>(target, 8,
        Request::PHYSICAL | Request::ATOMIC_RETURN_OP,
        system->getRequestorId(this));
    update->setAtomicOpFunctor(
        std::make_unique<paging::ConditionalUpdate<uint64_t>>(0x123, 0x80));
    update->setExtraData(0x123);
    auto *packet = new Packet(update, MemCmd::SwapReq);
    packet->allocate();
    port.submit(packet, [] { return false; },
        [=](PacketPtr result, bool cancelled) {
            panic_if(!cancelled || !result->isRequest(),
                     "Obsolete shared update reached memory");
            delete result;
            done(1);
        });
    auto makeRead = [=, this] {
        auto req = std::make_shared<Request>(target, 8, Request::PHYSICAL,
                                             system->getRequestorId(this));
        auto *packet = new Packet(req, MemCmd::ReadReq);
        packet->allocate();
        return packet;
    };
    port.submit(makeRead(), [] { return true; },
        [=, &port](PacketPtr result, bool cancelled) {
            panic_if(cancelled || result->getLE<uint64_t>() != 0x123,
                     "Shared transport returned wrong response or changed memory");
            delete result;
            done(2);
            // Submission inside a callback retains ownership and routing.
            port.submit(makeRead(), [] { return true; },
                [=](PacketPtr nested, bool cancelled) {
                    panic_if(cancelled || nested->getLE<uint64_t>() != 0x123,
                             "Reentrant shared client failed");
                    delete nested;
                    done(3);
                });
        });
    panic_if(port.outstanding() != 3,
             "Shared clients entered the blocked guest admission queue");
    // Functional traffic must work with the timing transport queue occupied.
    auto functional = std::make_shared<Request>(*req);
    panic_if(mmu->translateFunctional(functional, tc, BaseMMU::Read) ||
             functional->getPaddr() != Physical + (Virtual & 4095),
             "Functional translation failed with queued transport");
    for (unsigned l = 0; l < descriptors.size(); ++l)
        panic_if(descriptor(l) & 96, "Functional translation changed A/D");
}

void
PagingTester::snapshotContext()
{
    setup(Long);
    // Read is accepted with WP=1; write with WP=0. Neither queued request
    // may inherit the later CPL/CR3/CR4/EFER/APIC or request flag changes.
    descriptor(descriptors.size() - 1,
               (descriptor(descriptors.size() - 1) & ~6ULL) | (1ULL << 63));
    const RegVal oldApic = tc->readMiscRegNoEffect(misc_reg::ApicBase);
    LocalApicBase apic = oldApic;
    apic.base = Physical >> 12;
    tc->setMiscRegNoEffect(misc_reg::ApicBase, apic);
    auto completed = std::make_shared<unsigned>(0);
    awaiting = true;
    for (unsigned i = 0; i < 3; ++i) {
        CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
        cr0.wp = i != 1;
        tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
        auto mode = i == 0 ? BaseMMU::Read :
                    i == 1 ? BaseMMU::Write : BaseMMU::Execute;
        auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
            system->getRequestorId(this), Virtual, tc->contextId());
        auto callback = std::make_unique<TestContinuation>(
            [=, this](const Fault &fault, const RequestPtr &result,
                      ThreadContext *, BaseMMU::Mode returnedMode) {
                panic_if(returnedMode != mode || result != req,
                         "Context completion changed the request or mode");
                if (i == 2) {
                    auto *page = dynamic_cast<PageFault *>(fault.get());
                    const auto expected = PageFault(Virtual, true,
                        BaseMMU::Execute, false, false);
                    panic_if(!page || page->describe() != expected.describe(),
                             "Execute walk lost captured NXE/CPL");
                } else {
                    panic_if(fault || result->getPaddr() !=
                        x86LocalAPICAddress(tc->contextId(), Virtual & 4095) ||
                        !result->isUncacheable(),
                        "Immutable context was not retained");
                }
                if (++*completed == 3) {
                    tc->setMiscRegNoEffect(misc_reg::ApicBase, oldApic);
                    awaiting = false;
                    schedule(nextEvent, curTick() + 1);
                }
            });
        auto *continuation = callback.get();
        continuations.push_back(std::move(callback));
        if (i == 0)
            injection = RetryRead;
        mmu->translateTiming(req, tc, continuation, mode);
        req->setFlags(Request::READ_MODIFY_WRITE);
    }
    tc->setMiscRegNoEffect(misc_reg::Cr3, 0);
    tc->setMiscRegNoEffect(misc_reg::Cr4, 0);
    tc->setMiscRegNoEffect(misc_reg::Efer, 0);
    HandyM5Reg handy = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    handy.cpl = 3;
    tc->setMiscRegNoEffect(misc_reg::M5Reg, handy);
    tc->setMiscRegNoEffect(misc_reg::ApicBase, oldApic);
}

void
PagingTester::squashBatch(bool update, bool issued)
{
    setup(Long);
    auto cancelled = std::make_shared<bool>(false);
    auto completed = std::make_shared<unsigned>(0);
    squash = [cancelled] { *cancelled = true; };
    injection = issued ? (update ? SquashUpdate : SquashRead) :
                         (update ? SquashRetryUpdate : SquashRetryRead);
    awaiting = true;
    // Exceed the default per-cycle queued-squash budget (four).
    for (unsigned i = 0; i < 10; ++i) {
        auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
            system->getRequestorId(this), Virtual, tc->contextId());
        auto seen = std::make_shared<bool>(false);
        auto callback = std::make_unique<TestContinuation>(
            [=, this](const Fault &fault, const RequestPtr &result,
                      ThreadContext *, BaseMMU::Mode mode) {
                panic_if(*seen || !dynamic_cast<ReExec *>(fault.get()) ||
                         result != req || mode != BaseMMU::Write,
                         "Squashed walk callback contract failed");
                *seen = true;
                if (++*completed == 10) {
                    panic_if(tlb->lookup(Virtual, false),
                             "Squashed walk published a translation");
                    panic_if(pendingPackets,
                             "Squashed request retired before its response");
                    for (unsigned l = 0; l < descriptors.size(); ++l) {
                        const auto expected = issued && update && l == 0 ? 32 : 0;
                        panic_if((descriptor(l) & 96) != expected,
                                 "Squashed update memory effects were incorrect");
                    }
                    awaiting = false;
                    schedule(nextEvent, curTick() + 1);
                }
            }, cancelled);
        auto *continuation = callback.get();
        continuations.push_back(std::move(callback));
        mmu->translateTiming(req, tc, continuation, BaseMMU::Write);
    }
}

void
PagingTester::buildCases()
{
    cases.push_back([this] {
        setup(Pae);
        auto *isa = static_cast<ISA *>(tc->getIsaPtr());
        const auto old = isa->paePdpte();
        for (unsigned slot = 0; slot < 4; ++slot) {
            for (unsigned bit : {1, 2, 5, 6, 7, 8, 48, 63}) {
                std::array<RegVal, 4> values = {
                    0x1100001, 0x1101001, 0x1102001, 0x1103001};
                values[slot] = 1 | (1ULL << bit);
                const auto generation = tlb->generation();
                auto fault = isa->loadPaePdpte(values);
                auto *gp = dynamic_cast<GeneralProtection *>(fault.get());
                panic_if(!gp,
                         "Invalid PDPTE did not return #GP(0)");
                panic_if(isa->paePdpte() != old ||
                         tlb->generation() != generation,
                         "Failed PDPTE load changed retained state");
            }
        }
        auto values = old;
        values[3] = ~1ULL;
        panic_if(isa->loadPaePdpte(values) || isa->paePdpte() != values,
                 "Nonpresent PDPTE reserved bits were not ignored");
        schedule(nextEvent, curTick() + 1);
    });
    for (bool functional : {false, true}) {
        cases.push_back([=, this] {
            setup(Pae);
            descriptor(0, 0); // Memory changes do not reload retained registers.
            translate(BaseMMU::Read, Virtual, Physical + (Virtual & 4095),
                      NoFault, functional);
        });
        cases.push_back([=, this] {
            setup(Pae);
            auto *isa = static_cast<ISA *>(tc->getIsaPtr());
            auto values = isa->paePdpte();
            values[Virtual >> 30] = 0;
            panic_if(isa->loadPaePdpte(values), "Nonpresent PDPTE rejected");
            translate(BaseMMU::Read, Virtual, 0,
                std::make_shared<PageFault>(Virtual, false, BaseMMU::Read,
                                            false, false), functional);
        });
    }
    for (auto format : {Long, Pae, Legacy, Pse}) {
        for (bool large : {false, true}) {
            if (format == Legacy && large)
                continue;
            for (bool functional : {false, true}) {
                for (auto mode : {BaseMMU::Read, BaseMMU::Write}) {
                    cases.push_back([=, this] {
                        setup(format, large);
                        const unsigned shift = large ?
                            (format == Pse ? 22 : 21) : 12;
                        translate(mode, Virtual, Physical | (Virtual & mask(shift)),
                            NoFault, functional, [=, this] {
                                for (unsigned l = 0; l < descriptors.size(); ++l) {
                                    if (format == Pae && l == 0)
                                        continue;
                                    panic_if(bool(descriptor(l) & 32) == functional,
                                             "Unexpected accessed bit");
                                }
                                const auto leaf = descriptors.size() - 1;
                                panic_if(bool(descriptor(leaf) & 64) !=
                                    (!functional && mode == BaseMMU::Write),
                                    "Unexpected dirty bit");
                                panic_if(functional && tlb->lookup(Virtual, false),
                                         "Functional translation filled TLB");
                            });
                    });
                }
                const unsigned levels = format == Long ? (large ? 3 : 4) :
                    format == Pae ? (large ? 2 : 3) : (large ? 1 : 2);
                for (unsigned level = format == Pae ? 1 : 0;
                     level < levels; ++level) {
                    for (bool user : {false, true}) {
                        cases.push_back([=, this] {
                            setup(format, large, user);
                            descriptor(level, descriptor(level) & ~(user ? 4 : 2));
                            const auto mode = user ? BaseMMU::Read : BaseMMU::Write;
                            translate(mode, Virtual, 0,
                                std::make_shared<PageFault>(Virtual, true, mode,
                                                            user, false),
                                functional, [this] {
                                    panic_if(descriptor(descriptors.size()-1) & 64,
                                             "Denied write set dirty bit");
                                });
                        });
                    }
                }
            }
        }
    }
    for (auto format : {Long, Pae, Pse}) {
        for (bool large : {false, true}) {
            for (bool functional : {false, true}) {
                cases.push_back([=, this] {
                    setup(format, large);
                    const unsigned leaf = descriptors.size() - 1;
                    // 64-bit paging physical address width is 48 in this
                    // fixture; PSE large bit 21 is always reserved.
                    const uint64_t reserved = format == Pse ?
                        (large ? 1ULL << 21 : 0) : 1ULL << 48;
                    descriptor(leaf, descriptor(leaf) | reserved);
                    const unsigned shift = large ? (format == Pse ? 22 : 21) : 12;
                    translate(BaseMMU::Read, Virtual,
                        Physical | (Virtual & mask(shift)), reserved ?
                        std::make_shared<PageFault>(Virtual, true,
                            BaseMMU::Read, false, true) : NoFault, functional);
                });
                if (format == Pae) {
                    cases.push_back([=, this] {
                        setup(format, large);
                        descriptor(1, descriptor(1) | (1ULL << 52));
                        translate(BaseMMU::Read, Virtual, 0,
                            std::make_shared<PageFault>(Virtual, true,
                                BaseMMU::Read, false, true), functional);
                    });
                }
            }
        }
    }
    for (bool functional : {false, true}) {
        cases.push_back([=, this] {
            setup(Long, false, true);
            descriptor(0, descriptor(0) & ~4ULL);
            accessFlags.set(Request::READ_MODIFY_WRITE);
            translate(BaseMMU::Read, Virtual, 0,
                std::make_shared<PageFault>(Virtual, true, BaseMMU::Write,
                                            true, false), functional);
        });
        cases.push_back([=, this] {
            setup(Long);
            descriptor(0, descriptor(0) & ~1ULL);
            accessFlags.set(Request::READ_MODIFY_WRITE);
            translate(BaseMMU::Read, Virtual, 0,
                std::make_shared<PageFault>(Virtual, false, BaseMMU::Write,
                                            false, false), functional);
        });
        cases.push_back([=, this] {
            setup(Long);
            descriptor(0, descriptor(0) & ~2ULL);
            accessFlags.set(Request::CLEAN);
            translate(BaseMMU::Write, Virtual, Physical + (Virtual & 4095),
                NoFault, functional, [this] {
                    panic_if(descriptor(3) & 64, "Cache clean set dirty bit");
                });
        });
    }
    // Populate as supervisor with NXE clear, then fault on a cached user fetch.
    cases.push_back([this] {
        setup(Long);
        descriptor(0, descriptor(0) & ~4ULL);
        Efer efer = tc->readMiscRegNoEffect(misc_reg::Efer);
        efer.nxe = 0;
        tc->setMiscRegNoEffect(misc_reg::Efer, efer);
        translate(BaseMMU::Read, Virtual, Physical + (Virtual & 4095));
    });
    cases.push_back([this] {
        HandyM5Reg handy = tc->readMiscRegNoEffect(misc_reg::M5Reg);
        handy.cpl = 3;
        tc->setMiscRegNoEffect(misc_reg::M5Reg, handy);
        translate(BaseMMU::Execute, Virtual, 0,
            std::make_shared<PageFault>(Virtual, true, BaseMMU::Read,
                                        true, false));
    });
    // Port proxies (GDB, pseudo-instructions) translate linear addresses as
    // a supervisor read: no CPL, write-protect or segment checks, and no
    // A/D or cache side effects. CPU functional requests keep their checks.
    for (auto format : {Long, Legacy}) {
        cases.push_back([=, this] {
            setup(format, false, true);
            descriptor(0, descriptor(0) & ~6ULL);
            SegAttr unusable = 0;
            unusable.unusable = 1;
            tc->setMiscRegNoEffect(misc_reg::segAttr(segment_idx::Es),
                                   unusable);
            for (auto mode : {BaseMMU::Read, BaseMMU::Write}) {
                auto ranges = mmu->translateFunctional(Virtual, 8, tc, mode, 0);
                for (const auto &range : *ranges) {
                    panic_if(range.fault != NoFault ||
                             range.paddr != Physical + (Virtual & 4095),
                             "Proxy translation enforced CPU access rights");
                }
            }
            for (unsigned l = 0; l < descriptors.size(); ++l)
                panic_if(descriptor(l) & 96, "Proxy translation changed A/D");
            panic_if(tlb->lookup(Virtual, false),
                     "Proxy translation filled the TLB");
            translate(BaseMMU::Read, Virtual, 0,
                std::make_shared<PageFault>(Virtual, true, BaseMMU::Read,
                                            true, false), true);
        });
    }
    if (transportCases) {
        for (auto format : {Long, Pae, Legacy, Pse}) {
            for (bool large : {false, true}) {
                if (large && format == Legacy)
                    continue;
                for (unsigned selector = 0; selector < 8; ++selector) {
                    cases.push_back([=, this] {
                        setup(format, large);
                        // Fresh physical table pages avoid UC/WB aliases with
                        // dirty lines retained by preceding fixture cases.
                        // Changing a page's memory type without cache cleanup
                        // is not a defined architectural outcome to test.
                        const Addr delta = 0x400000 + index * 0x10000;
                        for (unsigned l = 0; l < descriptors.size(); ++l) {
                            auto value = descriptor(l);
                            if (l + 1 < descriptors.size())
                                value += delta;
                            descriptors[l] += delta;
                            descriptor(l, value);
                        }
                        tc->setMiscRegNoEffect(misc_reg::Cr3, Tables + delta);
                        const unsigned leaf = descriptors.size() - 1;
                        const unsigned pat = large ? 12 : 7;
                        descriptor(leaf, descriptor(leaf) |
                            ((selector & 3) << 3) | ((uint64_t(selector >> 2)) << pat));
                        // Exercise PWT/PCD on each referring structure and on
                        // descriptor updates, not just the final leaf result.
                        for (unsigned l = 0; l < leaf; ++l)
                            descriptor(l, descriptor(l) | (((l + 1) & 3) << 3));
                        if (format == Pae) {
                            auto *isa = static_cast<ISA *>(tc->getIsaPtr());
                            auto retained = isa->paePdpte();
                            retained[(Virtual >> 30) & 3] = descriptor(0);
                            panic_if(isa->loadPaePdpte(retained), "PDPTE attributes rejected");
                        }
                        inspectAttributes = [=, this](PacketPtr packet) {
                            const auto found = std::find(descriptors.begin(),
                                descriptors.end(), packet->getAddr());
                            panic_if(found == descriptors.end(), "Unexpected descriptor address");
                            const auto level = found - descriptors.begin();
                            const auto referring = level ? descriptor(level - 1) :
                                tc->readMiscRegNoEffect(misc_reg::Cr3);
                            const auto attrs = PagingPort::attributes(packet);
                            panic_if(attrs.pwt != bool(referring & 8) ||
                                     attrs.pcd != bool(referring & 16),
                                     "Descriptor transport lost PWT/PCD");
                        };
                        const unsigned bits = large ? (format == Pse ? 22 : 21) : 12;
                        translate(BaseMMU::Read, Virtual,
                            Physical + (Virtual & ((1ULL << bits) - 1)),
                            NoFault, false, [=, this] {
                                const auto *entry = tlb->lookup(Virtual, false);
                                panic_if(!entry || entry->patIndex() != selector,
                                         "Stage-one result lost raw PAT selector");
                            });
                    });
                }
            }
        }
    }
    // Long 4 KiB PAT must not depend on address bit 12.
    for (bool addressBit : {false, true}) {
        for (bool pat : {false, true}) {
            cases.push_back([=, this] {
                setup(Long);
                descriptor(3, descriptor(3) | (addressBit ? 4096 : 0) |
                                               (pat ? 128 : 0));
                translate(BaseMMU::Read, Virtual,
                    Physical + (addressBit ? 4096 : 0) + (Virtual & 4095),
                    NoFault, false, [=, this] {
                        auto *entry = tlb->lookup(Virtual, false);
                        panic_if(!entry || bool(entry->patBit) != pat,
                                 "4 KiB PAT extraction failed");
                    });
            });
        }
    }
    cases.push_back([this] {
        setup(Long);
        auto handy = HandyM5Reg(tc->readMiscRegNoEffect(misc_reg::M5Reg));
        handy.paging = 0;
        tc->setMiscRegNoEffect(misc_reg::M5Reg, handy);
        CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
        cr0.pg = 0;
        tc->setMiscRegNoEffect(misc_reg::Cr0, cr0);
        translate(BaseMMU::Read, Virtual, Virtual, NoFault, true);
    });
    // Exercise the cache's structured tag independently of MOV CR3's
    // permitted conservative flushing policy and CPUID advertisement.
    cases.push_back([this] {
        setup(Long);
        for (unsigned shift : {12, 21, 22}) {
            tlb->flushAll();
            const Addr base = Virtual & ~mask(shift);
            TlbEntry first;
            first.logBytes = shift;
            first.paddr = Physical;
            TlbEntry second = first;
            second.paddr += 1ULL << shift;
            tlb->insert(base, first, {tc->contextId(), 9});
            tlb->insert(base, second, {tc->contextId(), 17});
            panic_if(tlb->lookup(Virtual, false, {tc->contextId(), 9})->paddr != first.paddr ||
                     tlb->lookup(Virtual, false, {tc->contextId(), 17})->paddr != second.paddr,
                     "PCID mappings alias");
            tlb->demapPage(Virtual, 9);
            panic_if(tlb->lookup(Virtual, false, {tc->contextId(), 9}) ||
                     !tlb->lookup(Virtual, false, {tc->contextId(), 17}), "PCID invalidation leaked");
        }
        schedule(nextEvent, curTick() + 1);
    });
    cases.push_back([this] {
        setup(Long);
        for (unsigned shift : {12, 21, 22}) {
            tlb->flushAll();
            const Addr base = Virtual & ~mask(shift);
            TlbEntry entry;
            entry.logBytes = shift;
            entry.paddr = Physical;
            tlb->insert(base, entry, {1, 9});
            entry.paddr += 1ULL << shift;
            tlb->insert(base, entry, {2, 9});
            entry.global = true;
            tlb->insert(base, entry, {1, 17});
            tlb->insert(base, entry, {2, 17});
            panic_if(tlb->lookup(Virtual, false, {1, 9})->paddr != Physical ||
                     tlb->lookup(Virtual, false, {2, 9})->paddr != entry.paddr,
                     "Logical-thread mappings alias");
            tlb->invalidatePage(Virtual, {1, 9});
            panic_if(tlb->lookup(Virtual, false, {1, 9}) ||
                     tlb->lookup(Virtual, false, {1, 17}) ||
                     !tlb->lookup(Virtual, false, {2, 9}) ||
                     !tlb->lookup(Virtual, false, {2, 17}),
                     "Invalidation crossed a logical-thread boundary");
        }
        schedule(nextEvent, curTick() + 1);
    });
    // Replace an observed descriptor immediately before its compare-and-OR
    // reaches real memory. A stale full-descriptor write loses this race.
    for (auto format : {Long, Pae, Legacy, Pse}) {
        for (bool deny : {false, true}) {
            cases.push_back([=, this] {
                setup(format);
                const auto leaf = descriptors.size() - 1;
                replacement = deny ? descriptor(leaf) & ~2ULL :
                    (Physical + 4096) | 7;
                injection = ReplaceLeaf;
                translate(BaseMMU::Write, Virtual,
                    Physical + 4096 + (Virtual & 4095), deny ?
                    std::make_shared<PageFault>(Virtual, true,
                        BaseMMU::Write, false, false) : NoFault, false,
                    [=, this] {
                        panic_if(injected != 1 || descriptor(leaf) !=
                            (replacement | (deny ? 0 : 96)),
                            "Descriptor replacement was overwritten");
                    });
            });
        }
    }
    if (system->isTimingMode()) {
        for (auto inject : {RetryRead, RetryUpdate, FunctionalDuringRead, InvalidateRead,
                            InvalidateUpdate, InvalidateRetryRead,
                            InvalidateRetryUpdate}) {
            cases.push_back([=, this] {
                setup(Long);
                injection = inject;
                const bool invalidated = inject != RetryRead &&
                                         inject != RetryUpdate &&
                                         inject != FunctionalDuringRead;
                translate(BaseMMU::Write, Virtual, Physical + (Virtual & 4095),
                    invalidated ? std::make_shared<ReExec>() : NoFault,
                    false, [=, this] {
                        panic_if(injected != 1, "Injection did not run");
                        panic_if(invalidated && tlb->lookup(Virtual, false),
                                 "Obsolete walk filled the TLB");
                        if (inject == InvalidateRetryUpdate)
                            panic_if(descriptor(0) & 32,
                                     "Obsolete rejected update reached memory");
                    });
            });
        }
        cases.push_back([this] { queuePair(false); });
        cases.push_back([this] { queuePair(true); });
        cases.push_back([this] { queuePae(); });
        cases.push_back([this] { reentrantRetry(); });
        if (transportCases) {
            cases.push_back([this] { sharedTransport(); });
            cases.push_back([this] { snapshotContext(); });
            cases.push_back([this] { squashBatch(false); });
            cases.push_back([this] { squashBatch(true); });
            cases.push_back([this] { squashBatch(false, true); });
            cases.push_back([this] { squashBatch(true, true); });
        }
        if (system->threads.size() > 1) {
            for (auto format : {Long, Pae, Legacy, Pse}) {
                for (bool large : {false, true}) {
                    if (format == Legacy && large)
                        continue;
                    cases.push_back([=, this] { concurrentUpdate(format, large); });
                }
            }
        }
    }
}
} // namespace gem5::X86ISA
