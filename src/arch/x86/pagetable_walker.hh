/*
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

#ifndef __ARCH_X86_PAGE_TABLE_WALKER_HH__
#define __ARCH_X86_PAGE_TABLE_WALKER_HH__

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/paging_port.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/tlb.hh"
#include "arch/x86/translation.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "params/X86PagetableWalker.hh"
#include "sim/clocked_object.hh"
#include "sim/faults.hh"
#include "sim/system.hh"

namespace gem5
{

class ThreadContext;

namespace X86ISA
{
    class Walker : public ClockedObject
    {
      public:
        struct Result
        {
            TlbEntry entry;
            TlbContext context;
            uint64_t generation;
        };

        // The MMU owns the request continuation. The walker returns only
        // stage-one information and never finalizes or completes a CPU access.
        class Completion
        {
          public:
            virtual ~Completion() = default;
            virtual bool squashed() const = 0;
            virtual void finish(const Fault &fault, const Result *result) = 0;
        };

      protected:
        PagingPort port;

        // State to track each walk of the page table
        class WalkerState
        {
          friend class Walker;
          private:
            enum State
            {
                Ready,
                Waiting,
                // Long mode
                LongPML4, LongPDP, LongPD, LongPTE,
                // PAE legacy mode
                PAEPD, PAEPTE,
                // Non PAE legacy mode with and without PSE
                PSEPD, PD, PTE
            };

          protected:
            Walker *walker;
            const TranslationContextPtr snapshot;
            State state;
            State nextState;
            int dataSize;
            bool enableNX;
            bool haveResult = false;
            CR3 cr3;
            CR4 cr4;
            Efer efer;
            bool inUser;
            bool writeProtect;
            unsigned physicalBits;
            uint64_t generation;
            TlbContext context;
            std::array<RegVal, 4> paePdpte;
            unsigned inflight;
            TlbEntry entry;
            Addr originalVaddr;
            PagingAttributes readAttributes;
            PagingAttributes updateAttributes;
            PacketPtr read = nullptr;
            std::vector<PacketPtr> writes;
            Fault timingFault;
            std::shared_ptr<Completion> completion;
            BaseMMU::Mode mode;
            bool functional;
            bool timing;
            bool started;
          public:
            WalkerState(Walker * _walker, std::shared_ptr<Completion> _completion,
                        TranslationContextPtr captured, bool _isFunctional = false) :
                walker(_walker), snapshot(std::move(captured)), state(Ready),
                nextState(Ready), inflight(0),
                completion(std::move(_completion)),
                functional(_isFunctional), timing(false),
                started(false)
            {
            }
            ~WalkerState()
            {
                delete read;
                for (auto *packet : writes)
                    delete packet;
            }
            void initState(bool isTiming = false);
            Fault startWalk();
            Fault startFunctional(Addr &addr, unsigned &logBytes);
            bool recvPacket(PacketPtr pkt, bool cancelled);
            unsigned numInflight() const;
            bool wasStarted();
            bool isTiming();
            bool obsolete() const
            { return generation != walker->tlb->generation(); }
            std::string name() const {return walker->name();}

          private:
            Fault setupWalk(Addr vaddr);
            Fault stepWalk(PacketPtr &write);
            void sendPackets();
            void endWalk();
            void finishUpdate(PacketPtr packet);
            Result result() const { return {entry, context, generation}; }
            bool reserved(PageTableEntry pte) const;
            Fault pageFault(bool present, bool reserved = false);
        };

        friend class WalkerState;
        // State for timing and atomic accesses (need multiple per walker in
        // the case of multiple outstanding requests in timing mode)
        std::list<WalkerState *> currStates;

      public:
        DrainState drain() override;

        // Kick off the state machine.
        Fault start(TranslationContextPtr context,
                std::shared_ptr<Completion> completion,
                Result *atomicResult = nullptr);
        Fault startFunctional(TranslationContextPtr context, TlbEntry &result);
        Port &getPort(const std::string &if_name,
                      PortID idx=InvalidPortID) override;

      protected:
        // The TLB we're supposed to load.
        TLB * tlb;
        System * sys;
        RequestorID requestorId;

        // The number of outstanding walks that can be squashed per cycle.
        unsigned numSquashable;

        // Wrapper for checking for squashes before starting a translation.
        void startWalkWrapper();

        /**
         * Event used to call startWalkWrapper.
         **/
        EventFunctionWrapper startWalkWrapperEvent;

        // Functions for dealing with packets.
        void recvPacket(WalkerState *state, PacketPtr pkt, bool cancelled);
        void sendTiming(WalkerState *state, PacketPtr pkt);
        void checkDrain();

      public:

        PagingPort &pagingPort() { return port; }

        void setTLB(TLB * _tlb)
        {
            tlb = _tlb;
        }

        using Params = X86PagetableWalkerParams;

        Walker(const Params &params) :
            ClockedObject(params), port(name() + ".port", *this, [this] { checkDrain(); }),
            tlb(NULL), sys(params.system),
            requestorId(sys->getRequestorId(this)),
            numSquashable(params.num_squash_per_cycle),
            startWalkWrapperEvent([this]{ startWalkWrapper(); }, name())
        {
        }
    };

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_PAGE_TABLE_WALKER_HH__
