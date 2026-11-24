/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/paging_port.hh"

#include "sim/sim_object.hh"

namespace gem5
{
namespace X86ISA
{

PagingPort::PagingPort(const std::string &name, SimObject &owner,
                       std::function<void()> idle)
    : QueuedRequestPort(name, requests, snoops),
      requests(owner, *this),
      snoops(owner, *this),
      onIdle(std::move(idle)),
      traffic(&owner)
{}

PagingPort::Traffic::Traffic(statistics::Group *parent)
    : statistics::Group(parent, "traffic"),
      ADD_STAT(guestReads, statistics::units::Count::get(),
               "Guest descriptor reads"),
      ADD_STAT(eptReads, statistics::units::Count::get(),
               "EPT descriptor reads"),
      ADD_STAT(guestUpdates, statistics::units::Count::get(),
               "Guest A/D update requests"),
      ADD_STAT(eptUpdates, statistics::units::Count::get(),
               "EPT A/D update requests"),
      ADD_STAT(retries, statistics::units::Count::get(),
               "Rejected descriptor sends")
{}

void
PagingPort::Traffic::accepted(PagingStage stage, bool update)
{
    if (stage == PagingStage::Guest) {
        ++(update ? guestUpdates : guestReads);
    } else if (stage == PagingStage::Ept) {
        ++(update ? eptUpdates : eptReads);
    }
}

PagingPort::SenderState &
PagingPort::sender(PacketPtr packet)
{
    auto *state = dynamic_cast<SenderState *>(packet->senderState);
    assert(state);
    return *state;
}

void
PagingPort::submit(PacketPtr packet, Valid valid, Completion completion,
                   PagingAttributes attributes, PagingStage stage)
{
    assert(packet->isRequest() && packet->needsResponse());
    packet->pushSenderState(new SenderState(
        attributes, stage, std::move(valid), std::move(completion)));
    ++owned;
    requests.submit(packet);
}

Tick
PagingPort::sendAtomic(PacketPtr packet, PagingAttributes attributes,
                       PagingStage stage)
{
    packet->pushSenderState(new SenderState(attributes, stage, {}, {}));
    traffic.accepted(stage, packet->req->isAtomicReturn());
    const Tick latency = RequestPort::sendAtomic(packet);
    delete packet->popSenderState();
    return latency;
}

void
PagingPort::sendFunctional(PacketPtr packet, PagingAttributes attributes)
{
    packet->pushSenderState(
        new SenderState(attributes, PagingStage::Other, {}, {}));
    RequestPort::sendFunctional(packet);
    delete packet->popSenderState();
}

void
PagingPort::complete(PacketPtr packet, bool cancelled)
{
    auto completion = std::move(sender(packet).completion);
    delete packet->popSenderState();
    completion(packet, cancelled);
    // The callback may submit further work or inspect drain state.
    assert(owned);
    if (--owned == 0)
        onIdle();
}

bool
PagingPort::recvTimingResp(PacketPtr packet)
{
    complete(packet, false);
    return true;
}

bool
PagingPort::RequestQueue::sendTiming(PacketPtr packet)
{
    if (!sender(packet).valid()) {
        port.complete(packet, true);
        return true;
    }
    const auto stage = sender(packet).stage;
    const bool update = packet->req->isAtomicReturn();
    if (!port.sendTimingReq(packet)) {
        if (stage != PagingStage::Other) {
            ++port.traffic.retries;
        }
        return false;
    }
    port.traffic.accepted(stage, update);
    return true;
}

void
PagingPort::RequestQueue::enqueueReentrant()
{
    sending = false;
    for (auto *packet : reentrant)
        schedSendTiming(packet, curTick());
    reentrant.clear();
}

void
PagingPort::RequestQueue::sendDeferredPacket()
{
    assert(!sending);
    sending = true;
    ReqPacketQueue::sendDeferredPacket();
    enqueueReentrant();
}

void
PagingPort::RequestQueue::submit(PacketPtr packet)
{
    if (sending) {
        reentrant.push_back(packet);
        return;
    }
    // Keep the existing no-contention walk latency. Cancellation is always
    // deferred: completion must not destroy an initiating walk on its stack.
    if (size() == 0 && !waitingOnRetry && sender(packet).valid()) {
        sending = true;
        waitingOnRetry = !sendTiming(packet);
        if (waitingOnRetry)
            schedSendTiming(packet, curTick());
        enqueueReentrant();
    } else {
        schedSendTiming(packet, curTick());
    }
}

} // namespace X86ISA
} // namespace gem5
