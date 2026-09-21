/*
 * Copyright (c) 2020 ARM Limited
 * All rights reserved
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

#ifndef __ARCH_X86_MMU_HH__
#define __ARCH_X86_MMU_HH__

#include "arch/generic/mmu.hh"
#include "arch/x86/page_size.hh"
#include "arch/x86/tlb.hh"
#include "arch/x86/translation.hh"
#include "arch/x86/regs/misc.hh"
#include "base/addr_range.hh"
#include "cpu/thread_context.hh"

#include "params/X86MMU.hh"

namespace gem5
{

namespace X86ISA {

class MMU : public BaseMMU
{
  private:
    AddrRange m5opRange;
    unsigned pendingWalks = 0;
    class WalkContinuation;

    Fault translateInt(bool read, RequestPtr req, ThreadContext *tc);
    Fault translate(const RequestPtr &req, ThreadContext *tc,
                    Translation *translation, Mode mode,
                    bool &delayedResponse, bool timing, bool functional,
                    TLB &cache);
    TranslationContextPtr captureContext(const RequestPtr &req,
        ThreadContext *tc, TLB &cache, Mode access, Mode original,
        Addr linear, Addr faultAddress) const;
    Fault finishWalk(const RequestPtr &req, const TranslationContext &context,
                     const TlbEntry &entry);
    Fault finalizePhysical(const RequestPtr &req, Mode mode,
                           LocalApicBase apicBase, ContextID thread) const;

  public:
    MMU(const X86MMUParams &p);

    DrainState drain() override;
    Fault translateAtomic(const RequestPtr &req, ThreadContext *tc,
                          Mode mode) override;
    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         Translation *translation, Mode mode) override;
    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              Mode mode) override;
    Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                           Mode mode) const override;

    void
    flushNonGlobal()
    {
        static_cast<TLB*>(itb)->flushNonGlobal();
        if (dtb != itb)
            static_cast<TLB*>(dtb)->flushNonGlobal();
    }

    void
    invalidateLinear(Addr address, ThreadContext *tc)
    {
        CR4 cr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
        CR3 cr3 = tc->readMiscRegNoEffect(misc_reg::Cr3);
        TlbContext context{tc->contextId(), cr4.pcide ? uint64_t(cr3.pcid) : 0};
        static_cast<TLB*>(itb)->invalidatePage(address, context);
        if (dtb != itb)
            static_cast<TLB*>(dtb)->invalidatePage(address, context);
    }

    using BaseMMU::translateFunctional;

    Walker*
    getDataWalker()
    {
        return static_cast<TLB*>(dtb)->getWalker();
    }

    TranslationGenPtr
    translateFunctional(Addr start, Addr size, ThreadContext *tc,
            Mode mode, Request::Flags flags) override
    {
        return TranslationGenPtr(new MMUTranslationGen(
                PageBytes, start, size, tc, this, mode, flags));
    }
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_MMU_HH__
