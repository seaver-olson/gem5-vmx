/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/paging_port.hh"

namespace gem5
{
namespace X86ISA
{

PagingPort::PagingPort(const std::string &name, EventManager &em,
                       std::function<void()> idle)
    : QueuedRequestPort(name, requests, snoops), requests(em, *this),
      snoops(em, *this), onIdle(std::move(idle))
{}

PagingPort::SenderState &
PagingPort::sender(PacketPtr packet)
{
    auto *state = dynamic_cast<SenderState *>(packet->senderState);
    assert(state);
    return *state;
}

void
PagingPort::submit(PacketPtr packet, Valid valid, Completion completion,
                   PagingAttributes attributes)
{
    assert(packet->isRequest() && packet->needsResponse());
    packet->pushSenderState(new SenderState(attributes, std::move(valid),
                                            std::move(completion)));
    ++owned;
    requests.submit(packet);
}

Tick
PagingPort::sendAtomic(PacketPtr packet, PagingAttributes attributes)
{
    packet->pushSenderState(new SenderState(attributes, {}, {}));
    const Tick latency = RequestPort::sendAtomic(packet);
    delete packet->popSenderState();
    return latency;
}

void
PagingPort::sendFunctional(PacketPtr packet, PagingAttributes attributes)
{
    packet->pushSenderState(new SenderState(attributes, {}, {}));
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
    return port.sendTimingReq(packet);
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
