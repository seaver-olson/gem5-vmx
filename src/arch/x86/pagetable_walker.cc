/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/x86/pagetable_walker.hh"

#include <memory>

#include "arch/x86/faults.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/ldstflags.hh"
#include "arch/x86/paging.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/tlb.hh"
#include "base/bitfield.hh"
#include "base/trie.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/PageTableWalker.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"

namespace gem5
{

namespace X86ISA {

DrainState
Walker::drain()
{
    return currStates.empty() && port.idle() &&
        !startWalkWrapperEvent.scheduled() ?
        DrainState::Drained : DrainState::Draining;
}

Fault
Walker::start(TranslationContextPtr context,
              std::shared_ptr<Completion> completion,
              Result *atomicResult)
{
    if (!completion) {
        assert(atomicResult);
        WalkerState state(this, nullptr, std::move(context));
        state.initState();
        Fault fault = state.startWalk();
        if (fault == NoFault)
            *atomicResult = state.result();
        return fault;
    }
    // TODO: in timing mode, instead of blocking when there are other
    // outstanding requests, see if this request can be coalesced with
    // another one (i.e. either coalesce or start walk)
    WalkerState * newState = new WalkerState(this, std::move(completion), std::move(context));
    newState->initState(true);
    if (currStates.size()) {
        assert(newState->isTiming());
        DPRINTF(PageTableWalker, "Walks in progress: %d\n", currStates.size());
        currStates.push_back(newState);
        return NoFault;
    } else {
        currStates.push_back(newState);
        Fault fault = newState->startWalk();
        if (!newState->isTiming() || fault != NoFault) {
            currStates.pop_front();
            delete newState;
        }
        return fault;
    }
}

Fault
Walker::startFunctional(TranslationContextPtr context, TlbEntry &result)
{
    Addr address = context->linearAddress;
    WalkerState functionalState(this, nullptr, std::move(context), true);
    functionalState.initState();
    unsigned logBytes;
    Fault fault = functionalState.startFunctional(address, logBytes);
    if (fault == NoFault)
        result = functionalState.entry;
    return fault;
}

void
Walker::checkDrain()
{
    if (drain() == DrainState::Drained)
        signalDrainDone();
}

void
Walker::recvPacket(WalkerState *state, PacketPtr pkt, bool cancelled)
{
    if (state->recvPacket(pkt, cancelled)) {
        currStates.remove(state);
        delete state;
        if (!currStates.empty() && !currStates.front()->wasStarted() &&
            !startWalkWrapperEvent.scheduled())
            schedule(startWalkWrapperEvent, clockEdge());
        checkDrain();
    }
}

void
Walker::sendTiming(WalkerState *state, PacketPtr pkt)
{
    state->snapshot->port->submit(pkt,
        [state] { return !state->obsolete() &&
                        !state->completion->squashed(); },
        [this, state](PacketPtr response, bool cancelled) {
            recvPacket(state, response, cancelled);
        }, pkt->req->isAtomicReturn() ? state->updateAttributes :
                                       state->readAttributes);
}

Port &
Walker::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    else
        return ClockedObject::getPort(if_name, idx);
}

void
Walker::WalkerState::initState(bool isTiming)
{
    assert(state == Ready);
    started = false;
    mode = snapshot->accessMode;
    timing = isTiming;
    generation = snapshot->generation;
    cr3 = snapshot->cr3;
    cr4 = snapshot->cr4;
    context = snapshot->tag;
    efer = snapshot->efer;
    inUser = snapshot->m5reg.cpl == 3 && !(snapshot->flags & CPL0FlagBit);
    writeProtect = snapshot->cr0.wp;
    paePdpte = snapshot->paePdpte;
    physicalBits = snapshot->physicalBits;
    originalVaddr = snapshot->faultAddress;
}

void
Walker::startWalkWrapper()
{
    unsigned num_squashed = 0;
    WalkerState *currState = currStates.empty() ? nullptr : currStates.front();
    while ((num_squashed < numSquashable) && currState &&
        !currState->wasStarted() &&
        (currState->completion->squashed() || currState->obsolete())) {
        currStates.pop_front();
        num_squashed++;

        DPRINTF(PageTableWalker, "Squashing table walk for address %#x\n",
            currState->snapshot->linearAddress);

        currState->completion->finish(std::make_shared<ReExec>(), nullptr);

        // Only queued, unstarted work can retire here. A completion may
        // reenter start() and immediately issue another walk. Keep such
        // active work in currStates until its response/retry retires it,
        // so drain continues to account for its packet ownership.
        assert(currState->numInflight() == 0);
        delete currState;

        // check the next translation request, if it exists
        if (currStates.size())
            currState = currStates.front();
        else
            currState = NULL;
    }
    if (currState && !currState->wasStarted()) {
        if (currState->completion->squashed() || currState->obsolete())
            schedule(startWalkWrapperEvent, clockEdge(Cycles(1)));
        else {
            Fault fault = currState->startWalk();
            if (fault != NoFault) {
                currStates.pop_front();
                currState->completion->finish(fault, nullptr);
                delete currState;
                if (!currStates.empty() && !startWalkWrapperEvent.scheduled())
                    schedule(startWalkWrapperEvent, clockEdge(Cycles(1)));
            }
        }
    }
    checkDrain();
}

Fault
Walker::WalkerState::startWalk()
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    fault = setupWalk(snapshot->linearAddress);
    if (fault != NoFault)
        return fault;
    if (timing) {
        nextState = state;
        state = Waiting;
        timingFault = NoFault;
        sendPackets();
    } else {
        do {
            snapshot->port->sendAtomic(read, readAttributes);
            PacketPtr write = NULL;
            fault = stepWalk(write);
            assert(fault == NoFault || read == NULL);
            state = nextState;
            nextState = Ready;
            if (write) {
                snapshot->port->sendAtomic(write, updateAttributes);
                finishUpdate(write);
                delete write;
            }
        } while (read);
        if (fault == NoFault && obsolete())
            fault = std::make_shared<ReExec>();
        state = Ready;
        nextState = Waiting;
    }
    return fault;
}

Fault
Walker::WalkerState::startFunctional(Addr &addr, unsigned &logBytes)
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    fault = setupWalk(addr);
    if (fault != NoFault)
        return fault;

    do {
        snapshot->port->sendFunctional(read, readAttributes);
        // On a functional access (page table lookup), writes should
        // not happen so this pointer is ignored after stepWalk
        PacketPtr write = NULL;
        fault = stepWalk(write);
        assert(fault == NoFault || read == NULL);
        // delete the write packet if it exists
        if (write) {
            delete write;
        }
        state = nextState;
        nextState = Ready;
    } while (read);
    logBytes = entry.logBytes;
    addr = entry.paddr;

    return fault;
}

Fault
Walker::WalkerState::stepWalk(PacketPtr &write)
{
    assert(state != Ready && state != Waiting);
    Fault fault = NoFault;
    write = NULL;
    PageTableEntry pte;
    if (dataSize == 8)
        pte = read->getLE<uint64_t>();
    else
        pte = read->getLE<uint32_t>();
    const uint64_t observed = pte;
    VAddr vaddr = entry.vaddr;
    bool uncacheable = pte.pcd;
    Addr nextRead = 0;
    bool doWrite = false;
    bool doTLBInsert = false;
    bool doEndWalk = false;
    bool badNX = pte.nx && mode == BaseMMU::Execute && enableNX;
    if (pte.p && reserved(pte)) {
        fault = pageFault(true, true);
        endWalk();
        return fault;
    }
    if (dataSize == 8)
        entry.noExec = entry.noExec || (enableNX && pte.nx);
    switch(state) {
      case LongPML4:
        DPRINTF(PageTableWalker, "Got long mode PML4 entry %#016x.\n", pte);
        nextRead = mbits(pte, 51, 12) + vaddr.longl3 * dataSize;
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        nextState = LongPDP;
        break;
      case LongPDP:
        DPRINTF(PageTableWalker, "Got long mode PDP entry %#016x.\n", pte);
        nextRead = mbits(pte, 51, 12) + vaddr.longl2 * dataSize;
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        nextState = LongPD;
        break;
      case LongPD:
        DPRINTF(PageTableWalker, "Got long mode PD entry %#016x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (!pte.ps) {
            // 4 KB page
            entry.logBytes = 12;
            nextRead = mbits(pte, 51, 12) + vaddr.longl1 * dataSize;
            nextState = LongPTE;
            break;
        } else {
            // 2 MB page
            entry.logBytes = 21;
            entry.paddr = mbits(pte, 51, 21);
            entry.uncacheable = uncacheable;
            entry.pwt = pte.pwt;
            entry.pcd = pte.pcd;
            entry.global = cr4.pge && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 21);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
      case LongPTE:
        DPRINTF(PageTableWalker, "Got long mode PTE entry %#016x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        entry.paddr = mbits(pte, 51, 12);
        entry.uncacheable = uncacheable;
        entry.pwt = pte.pwt;
        entry.pcd = pte.pcd;
        entry.global = cr4.pge && pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = mbits(entry.vaddr, 63, 12);
        doTLBInsert = true;
        doEndWalk = true;
        break;
      case PAEPD:
        DPRINTF(PageTableWalker, "Got legacy mode PAE PD entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (!pte.ps) {
            // 4 KB page
            entry.logBytes = 12;
            nextRead = mbits(pte, 51, 12) + vaddr.pael1 * dataSize;
            nextState = PAEPTE;
            break;
        } else {
            // 2 MB page
            entry.logBytes = 21;
            entry.paddr = mbits(pte, 51, 21);
            entry.uncacheable = uncacheable;
            entry.pwt = pte.pwt;
            entry.pcd = pte.pcd;
            entry.global = cr4.pge && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 21);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
      case PAEPTE:
        DPRINTF(PageTableWalker,
                "Got legacy mode PAE PTE entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX || !pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        entry.paddr = mbits(pte, 51, 12);
        entry.uncacheable = uncacheable;
        entry.pwt = pte.pwt;
        entry.pcd = pte.pcd;
        entry.global = cr4.pge && pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = mbits(entry.vaddr, 63, 12);
        doTLBInsert = true;
        doEndWalk = true;
        break;
      case PSEPD:
        DPRINTF(PageTableWalker, "Got legacy mode PSE PD entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        if (!pte.ps) {
            // 4 KB page
            entry.logBytes = 12;
            nextRead = mbits(pte, 31, 12) + vaddr.norml1 * dataSize;
            nextState = PTE;
            break;
        } else {
            // 4 MB page
            entry.logBytes = 22;
            entry.paddr = bits(pte, 20, 13) << 32 | mbits(pte, 31, 22);
            entry.uncacheable = uncacheable;
            entry.pwt = pte.pwt;
            entry.pcd = pte.pcd;
            entry.global = cr4.pge && pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = mbits(entry.vaddr, 63, 22);
            doTLBInsert = true;
            doEndWalk = true;
            break;
        }
      case PD:
        DPRINTF(PageTableWalker, "Got legacy mode PD entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        // 4 KB page
        entry.logBytes = 12;
        nextRead = mbits(pte, 31, 12) + vaddr.norml1 * dataSize;
        nextState = PTE;
        break;
      case PTE:
        DPRINTF(PageTableWalker, "Got legacy mode PTE entry %#08x.\n", pte);
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (!pte.p) {
            doEndWalk = true;
            fault = pageFault(pte.p);
            break;
        }
        entry.paddr = mbits(pte, 31, 12);
        entry.uncacheable = uncacheable;
        entry.pwt = pte.pwt;
        entry.pcd = pte.pcd;
        entry.global = cr4.pge && pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = mbits(entry.vaddr, 31, 12);
        doTLBInsert = true;
        doEndWalk = true;
        break;
      default:
        panic("Unknown page table walker state %d!\n");
    }
    if (doTLBInsert) {
        const bool storeCheck = (snapshot->flags & Request::READ_MODIFY_WRITE);
        const bool badWrite = !entry.writable && (inUser || writeProtect);
        if ((inUser && !entry.user) ||
            ((mode == BaseMMU::Write || storeCheck) && badWrite) ||
            (mode == BaseMMU::Execute && entry.noExec)) {
            fault = pageFault(true);
            if (storeCheck && badWrite)
                fault = std::make_shared<PageFault>(originalVaddr, true,
                    BaseMMU::Write, inUser, false);
        } else {
            if (mode == BaseMMU::Write) {
                doWrite = doWrite || !pte.d;
                pte.d = 1;
            }
            entry.dirty = pte.d;
            haveResult = true;
        }
    }

    // Complete every required update before reading the next descriptor or
    // publishing the leaf. This also gives a failed compare a clean restart.
    if (doWrite && fault == NoFault && !functional) {
        updateAttributes = readAttributes;
        RequestPtr update = std::make_shared<Request>(read->getAddr(),
            dataSize, read->req->getFlags() | Request::ATOMIC_RETURN_OP,
            walker->requestorId);
        if (dataSize == 8)
            update->setAtomicOpFunctor(
                std::make_unique<paging::ConditionalUpdate<uint64_t>>(
                    observed, uint64_t(pte) & ~observed));
        else
            update->setAtomicOpFunctor(
                std::make_unique<paging::ConditionalUpdate<uint32_t>>(
                    observed, uint64_t(pte) & ~observed));
        update->setExtraData(observed);
        write = new Packet(update, MemCmd::SwapReq);
        write->allocate();
    }
    if (doEndWalk) {
        endWalk();
    } else {
        Request::Flags flags = read->req->getFlags();
        readAttributes = {bool(pte.pwt), bool(pte.pcd)};
        flags.set(Request::UNCACHEABLE, uncacheable);
        delete read;
        RequestPtr request = std::make_shared<Request>(
            nextRead, dataSize, flags, walker->requestorId);
        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
    }
    return fault;
}

void
Walker::WalkerState::endWalk()
{
    nextState = Ready;
    delete read;
    read = NULL;
}

Fault
Walker::WalkerState::setupWalk(Addr vaddr)
{
    VAddr addr = vaddr;
    // Use the context captured when this request was accepted.
    dataSize = 8;
    nextState = Ready;
    haveResult = false;
    entry = TlbEntry();
    entry.vaddr = vaddr;
    bool uncacheable = !cr4.pcide && cr3.pcd;
    readAttributes = {!cr4.pcide && bool(cr3.pwt), uncacheable};
    Addr topAddr;
    if (efer.lma) {
        // Do long mode.
        state = LongPML4;
        topAddr = (cr3.longPdtb << 12) + addr.longl4 * dataSize;
        enableNX = efer.nxe;
    } else {
        // We're in some flavor of legacy mode.
        if (cr4.pae) {
            // Do legacy PAE.
            state = PAEPD;
            enableNX = efer.nxe;
            PageTableEntry pdpte = paePdpte[addr.pael3];
            if (!pdpte.p)
                return pageFault(false);
            topAddr = mbits(pdpte, 51, 12) + addr.pael2 * dataSize;
            uncacheable = pdpte.pcd;
            readAttributes = {bool(pdpte.pwt), bool(pdpte.pcd)};
        } else {
            dataSize = 4;
            topAddr = (cr3.pdtb << 12) + addr.norml2 * dataSize;
            if (cr4.pse) {
                // Do legacy PSE.
                state = PSEPD;
            } else {
                // Do legacy non PSE.
                state = PD;
            }
            enableNX = false;
        }
    }

    Request::Flags flags = Request::PHYSICAL;

    // PCD can't be used if CR4.PCIDE=1 [sec 2.5
    // of Intel's Software Developer's manual]
    if (uncacheable)
        flags.set(Request::UNCACHEABLE);

    RequestPtr request = std::make_shared<Request>(
        topAddr, dataSize, flags, walker->requestorId);

    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
    return NoFault;
}

bool
Walker::WalkerState::recvPacket(PacketPtr pkt, bool cancelled)
{
    assert(cancelled || pkt->isResponse());
    assert(inflight);
    assert(state == Waiting);
    inflight--;
    if (cancelled || obsolete() || completion->squashed()) {
        // An already-issued update may have completed; retain ownership
        // until its response, then discard all remaining work and retry.
        delete pkt;
        delete read;
        read = nullptr;
        for (auto *write : writes)
            delete write;
        writes.clear();
        timingFault = std::make_shared<ReExec>();
    } else if (pkt->req->isAtomicReturn()) {
        finishUpdate(pkt);
        delete pkt;
        sendPackets();
    } else if (pkt->isRead()) {
        // should not have a pending read it we also had one outstanding
        assert(!read);

        // @todo someone should pay for this
        pkt->headerDelay = pkt->payloadDelay = 0;

        state = nextState;
        nextState = Ready;
        PacketPtr write = NULL;
        read = pkt;
        timingFault = stepWalk(write);
        state = Waiting;
        assert(timingFault == NoFault || read == NULL);
        if (write) {
            writes.push_back(write);
        }
        sendPackets();
    } else {
        sendPackets();
    }
    if (inflight == 0 && read == NULL && writes.size() == 0) {
        state = Ready;
        nextState = Waiting;
        if (timingFault == NoFault) {
            assert(haveResult);
            const auto stageResult = result();
            completion->finish(NoFault, &stageResult);
        } else {
            completion->finish(timingFault, nullptr);
        }
        return true;
    }

    return false;
}

void
Walker::WalkerState::sendPackets()
{
    if (inflight)
        return;
    // Updates precede dependent descriptor reads and final completion.
    PacketPtr packet = nullptr;
    if (!writes.empty()) {
        packet = writes.back();
        writes.pop_back();
    } else if (read) {
        packet = read;
        read = nullptr;
    }
    if (!packet)
        return;
    ++inflight;
    walker->sendTiming(this, packet);

}

void
Walker::WalkerState::finishUpdate(PacketPtr packet)
{
    panic_if(packet->isError(), "Paging descriptor update failed");
    const uint64_t current = dataSize == 8 ? packet->getLE<uint64_t>() :
                                            packet->getLE<uint32_t>();
    if (current == packet->req->getExtraData())
        return;
    // A different CPU or software replaced this descriptor. Never publish
    // a translation derived from the superseded observation.
    delete read;
    read = nullptr;
    const Fault fault = setupWalk(snapshot->linearAddress);
    // Retried descriptor updates retain the same captured PDPTE registers.
    assert(fault == NoFault);
    if (timing) {
        nextState = state;
        state = Waiting;
    }
}

unsigned
Walker::WalkerState::numInflight() const
{
    return inflight;
}

bool
Walker::WalkerState::isTiming()
{
    return timing;
}

bool
Walker::WalkerState::wasStarted()
{
    return started;
}

bool
Walker::WalkerState::reserved(PageTableEntry pte) const
{
    if (dataSize == 8) {
        if (paging::reservedAddress(pte, physicalBits) ||
            (!enableNX && pte.nx))
            return true;
        if (state == LongPML4 || state == LongPDP)
            return pte.ps; // Guest 1 GiB pages are not supported.
        if ((state == PAEPD || state == PAEPTE) && mbits(pte, 62, 52))
            return true;
        if ((state == LongPD || state == PAEPD) && pte.ps)
            return mbits(pte, 20, 13) != 0;
    } else if (state == PSEPD && pte.ps) {
        // PSE-36 encodes address bits 39:32 in entry bits 20:13.
        const unsigned extension = std::min(physicalBits, 40u) - 32;
        return bits(pte, 21) ||
            (extension < 8 && bits(pte, 20, 13 + extension));
    }
    return false;
}

Fault
Walker::WalkerState::pageFault(bool present, bool reserved)
{
    DPRINTF(PageTableWalker, "Raising page fault.\n");
    // Keep the callback's original mode intact.
    auto faultMode = mode == BaseMMU::Execute && !enableNX ?
        BaseMMU::Read : mode;
    if (mode == BaseMMU::Read && (snapshot->flags & Request::READ_MODIFY_WRITE))
        faultMode = BaseMMU::Write;
    return std::make_shared<PageFault>(originalVaddr, present, faultMode,
                                      inUser, reserved);
}

} // namespace X86ISA
} // namespace gem5
