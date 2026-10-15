/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __ARCH_X86_PAGING_TESTER_HH__
#define __ARCH_X86_PAGING_TESTER_HH__

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "arch/generic/mmu.hh"
#include "mem/port.hh"
#include "params/X86PagingTester.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5
{
class System;
namespace X86ISA
{
class MMU;
class TLB;

// A paused CPU supplies a real ThreadContext. Faults are inspected, never
// invoked, so the fixture also covers unsupported legacy exception delivery.
class PagingTester : public SimObject, public BaseMMU::Translation
{
  private:
    enum Format { Long, Pae, Legacy, Pse };
    static constexpr Addr Virtual = 0x40003123;
    static constexpr Addr Physical = 0x2000000;
    static constexpr Addr Tables = 0x1000000;
    System *system;
    const bool transportCases;
    const bool eptOnly;
    Tick observedAtomicLatency = 0;
    class Upstream : public ResponsePort
    {
      private:
        PagingTester &owner;
      public:
        explicit Upstream(PagingTester &tester);
        AddrRangeList getAddrRanges() const override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
    } upstream;
    class Downstream : public RequestPort
    {
      private:
        PagingTester &owner;
      public:
        explicit Downstream(PagingTester &tester);
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvRangeChange() override;
    } downstream;
    enum Injection { None, ReplaceLeaf, InvalidateRead, InvalidateUpdate,
                     RetryRead, RetryUpdate, InvalidateRetryRead,
                     InvalidateRetryUpdate, FunctionalDuringRead,
                     PeerUpdate, SquashRetryRead, SquashRetryUpdate,
                     SquashRead, SquashUpdate } injection = None;
    uint64_t replacement = 0;
    unsigned injected = 0;
    unsigned pendingPackets = 0;
    unsigned failedUpdates = 0;
    std::function<void()> startPeer;
    std::function<void()> squash;
    std::function<void(PacketPtr)> inspectAttributes;
    std::function<void(PacketPtr)> inspectResponse;
    EventFunctionWrapper retryEvent;
    ThreadContext *tc = nullptr;
    MMU *mmu = nullptr;
    TLB *tlb = nullptr;
    EventFunctionWrapper nextEvent;
    std::vector<std::function<void()>> cases;
    std::vector<std::unique_ptr<BaseMMU::Translation>> continuations;
    std::vector<Addr> descriptors;
    unsigned descriptorSize = 8;
    unsigned index = 0;
    bool awaiting = false;
    uint64_t savedEptCapability = 0;
    RequestPtr request;
    Request::Flags accessFlags;
    BaseMMU::Mode expectedMode = BaseMMU::Read;
    Addr expectedAddress = 0;
    Fault expectedFault;
    std::function<void()> after;

    void setup(Format format, bool large = false, bool user = false);
    uint64_t descriptor(unsigned level) const;
    void descriptor(unsigned level, uint64_t value);
    void translate(BaseMMU::Mode mode, Addr address, Addr physical,
                   const Fault &fault = NoFault, bool functional = false,
                   std::function<void()> check = {});
    void buildCases();
    void eptCases();
    enum class EptTest { Mapping, DeniedRead, DeniedWrite, DeniedExecute,
                         TableRead, TableUpdate, DirtyUpdate, DataMisconfig,
                         TableMisconfig, GuestFault, Replace, Remap,
                         InheritedWrite, Rmw };
    void setupEpt(bool presetAD, bool eptAD = false);
    void finishEpt();
    void eptMmuCases();
    void eptMmuAccess(BaseMMU::Mode mode, bool functional, bool warm,
                      EptTest test, unsigned level = 0, bool primed = false);
    void eptMmuTiming(unsigned point, Injection inject, bool warm = false,
                      bool primed = false);
    enum class EptAdTest { Read, Write, Execute, Large2M, Large1G, FiveLevel,
                           AlreadySet, Replace, Revoke, Denied, Absent,
                           Misconfig, UpdateError,
                           Retry, CancelQueued, CancelIssued };
    void eptAdCase(EptAdTest test);
    void eptAdGuest(bool denied, bool functional, bool writing = false,
                    bool presetAD = true);
    void eptTimingLifecycle(bool retry, bool cancel, unsigned stop);
    void eptTimingConcurrent();
    void next();
    void replace(PacketPtr packet);
    bool sendTiming(PacketPtr packet);
    void sharedTransport();
    void snapshotContext();
    void squashBatch(bool update, bool issued = false);
    void queuePair(bool invalidate);
    void queuePae();
    void reentrantRetry();
    void concurrentUpdate(Format format, bool large);

  public:
    explicit PagingTester(const X86PagingTesterParams &params);
    void startup() override;
    Port &getPort(const std::string &name, PortID index = InvalidPortID) override;
    void markDelayed() override {}
    void finish(const Fault &fault, const RequestPtr &req,
                ThreadContext *context, BaseMMU::Mode mode) override;
};
} // namespace X86ISA
} // namespace gem5
#endif
