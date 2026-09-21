/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __ARCH_X86_PAGING_PORT_HH__
#define __ARCH_X86_PAGING_PORT_HH__

#include <functional>
#include <vector>

#include "mem/qport.hh"

namespace gem5
{
namespace X86ISA
{

// Raw attributes of the structure referring to a guest descriptor. These
// survive transport without conflating PWT with the current cache model.
struct PagingAttributes
{
    bool pwt = false;
    bool pcd = false;
};

// Physical transport shared by guest and nested walkers of one access stream.
// Admission does not depend on the guest walk queue. Callbacks own packets
// after completion, including requests cancelled before reaching memory.
class PagingPort : public QueuedRequestPort
{
  public:
    using Valid = std::function<bool()>;
    using Completion = std::function<void(PacketPtr, bool cancelled)>;

  private:
    struct SenderState : Packet::SenderState
    {
        PagingAttributes attributes;
        Valid valid;
        Completion completion;
        SenderState(PagingAttributes attrs, Valid check, Completion done)
            : attributes(attrs), valid(std::move(check)),
              completion(std::move(done)) {}
    };

    class RequestQueue : public ReqPacketQueue
    {
      private:
        PagingPort &port;
        bool sending = false;
        std::vector<PacketPtr> reentrant;
        void enqueueReentrant();
      protected:
        bool sendTiming(PacketPtr packet) override;
        void sendDeferredPacket() override;
      public:
        RequestQueue(EventManager &em, PagingPort &owner)
            : ReqPacketQueue(em, owner), port(owner) {}
        void submit(PacketPtr packet);
    } requests;
    SnoopRespPacketQueue snoops;
    size_t owned = 0;
    std::function<void()> onIdle;

    static SenderState &sender(PacketPtr packet);
    void complete(PacketPtr packet, bool cancelled);
    bool recvTimingResp(PacketPtr packet) override;

  public:
    PagingPort(const std::string &name, EventManager &em,
               std::function<void()> idle);
    void submit(PacketPtr packet, Valid valid, Completion completion,
                PagingAttributes attributes = {});
    Tick sendAtomic(PacketPtr packet, PagingAttributes attributes = {});
    void sendFunctional(PacketPtr packet, PagingAttributes attributes = {});
    static PagingAttributes attributes(PacketPtr packet)
    {
        auto *state = packet->findNextSenderState<SenderState>();
        assert(state);
        return state->attributes;
    }
    // Includes queued requests, issued requests, and executing callbacks.
    bool idle() const { return owned == 0; }
    size_t outstanding() const { return owned; }
};

} // namespace X86ISA
} // namespace gem5
#endif
