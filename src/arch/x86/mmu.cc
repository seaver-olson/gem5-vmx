/*
 * Copyright (c) 2007-2008 The Hewlett-Packard Development Company
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

#include "arch/x86/mmu.hh"

#include <cstring>
#include <memory>

#include "arch/x86/ept_walker.hh"
#include "arch/x86/faults.hh"
#include "arch/x86/insts/microldstop.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/paging.hh"
#include "arch/x86/pseudo_inst_abi.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/regs/msr.hh"
#include "arch/x86/x86_traits.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "mem/packet_access.hh"
#include "mem/page_table.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace X86ISA {

MMU::MMU(const X86MMUParams &p)
    : BaseMMU(p), m5opRange(p.system->m5opRange())
{}

DrainState
MMU::drain()
{
    return pendingWalks ? DrainState::Draining : DrainState::Drained;
}

TranslationContextPtr
MMU::captureContext(const RequestPtr &req, ThreadContext *tc, TLB &cache,
                    Mode access, Mode original, Addr linear,
                    Addr faultAddress, bool supervisor) const
{
    auto context = std::make_shared<TranslationContext>();
    context->cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
    context->cr3 = tc->readMiscRegNoEffect(misc_reg::Cr3);
    context->cr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
    context->efer = tc->readMiscRegNoEffect(misc_reg::Efer);
    context->m5reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    context->pat = tc->readMiscRegNoEffect(misc_reg::Pat);
    context->apicBase = tc->readMiscRegNoEffect(misc_reg::ApicBase);
    context->tag = {tc->contextId(), context->cr4.pcide ?
        uint64_t(context->cr3.pcid) : 0};
    context->generation = cache.generation(context->tag.thread);
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    context->ept = isa->vmxState().eptConfig();
    context->eptCapabilities =
        isa->readMiscRegNoEffect(misc_reg::VmxEptVpidCap);
    context->paePdpte = isa->paePdpte();
    CpuidResult cpuid;
    fatal_if(!isa->cpuid->doCpuid(tc, 0x80000008, 0, cpuid),
             "Missing physical address width CPUID");
    context->physicalBits = bits(cpuid.rax, 7, 0);
    fatal_if(context->physicalBits < 32 || context->physicalBits > 52,
             "Unsupported x86 physical address width %u", context->physicalBits);
    context->linearAddress = linear;
    context->faultAddress = faultAddress;
    context->flags = req->getFlags();
    if (supervisor)
        context->flags.set(CPL0FlagBit);
    context->accessMode = access;
    context->originalMode = original;
    context->port = &cache.getWalker()->pagingPort();
    return context;
}

namespace
{

EptAccess
finalAccess(BaseMMU::Mode mode, Request::Flags flags)
{
    if (mode == BaseMMU::Execute) {
        return EptAccess::Execute;
    }
    if (mode == BaseMMU::Write || (flags & Request::READ_MODIFY_WRITE)) {
        return EptAccess::Write;
    }
    return EptAccess::Read;
}

CombinedTag
combinedTag(TlbContext tag, CR3 cr3, CR4 cr4, HandyM5Reg handy,
            const EptConfig &config)
{
    const Addr root = handy.mode != LongMode && cr4.pae
                          ? Addr(cr3.paePdtb) << 5
                          : Addr(cr3.longPdtb) << 12;
    return {tag.thread, root, tag.addressSpace, config.eptp};
}

// Copy stage one before any asynchronous final lookup. A guest TLB slot
// can be replaced while that lookup is in flight.
std::optional<CombinedEntry>
candidateFor(const TranslationContext &context, const TlbEntry *guest,
             const TLB &cache)
{
    if (!guest || !cache.combined.enabled()) {
        return std::nullopt;
    }

    CombinedEntry entry{};
    entry.tag = combinedTag(context.tag, context.cr3, context.cr4,
                            context.m5reg, *context.ept);
    entry.shift = guest->logBytes;
    entry.writable = guest->writable;
    entry.user = guest->user;
    entry.noExec = guest->noExec;
    entry.guestDirty = guest->dirty;
    entry.patIndex = guest->patIndex();
    return entry;
}

void
applyFinalType(const RequestPtr &req, CR0 cr0, RegVal pat, uint8_t index,
               uint8_t ept_type, bool ignore_pat, EptAccess access)
{
    const auto guest_type = cr0.pg ? paging::patType(pat, index) : 6;
    paging::applyMemoryType(
        *req,
        paging::effectiveEptType(cr0.cd, ept_type, ignore_pat, guest_type),
        access == EptAccess::Write);
}

} // anonymous namespace

class MMU::WalkContinuation :
    public Walker::Completion,
    public std::enable_shared_from_this<WalkContinuation>
{
  private:
    MMU &mmu;
    TLB &cache;
    RequestPtr req;
    ThreadContext *tc;
    Translation *translation;
    const TranslationContextPtr snapshot;
    const Tick start = curTick();
    bool completed = false;

  public:
    WalkContinuation(MMU &owner, TLB &target, const RequestPtr &request,
                     ThreadContext *context, Translation *callback,
                     TranslationContextPtr captured)
        : mmu(owner), cache(target), req(request), tc(context),
          translation(callback), snapshot(std::move(captured))
    {
        assert(translation);
        ++mmu.pendingWalks;
    }

    ~WalkContinuation() override
    {
        assert(mmu.pendingWalks);
        if (--mmu.pendingWalks == 0)
            mmu.signalDrainDone();
    }

    bool squashed() const override
    {
        return completed || translation->squashed();
    }

    bool done() const { return completed; }

    void
    startFinal(Addr guestPhysical, const TlbEntry *guest)
    {
        req->setPaddr(guestPhysical);
        if (!snapshot->ept) {
            complete(mmu.finalizePhysical(req, snapshot->accessMode,
                snapshot->apicBase, snapshot->tag.thread));
            return;
        }
        const auto access = finalAccess(snapshot->accessMode, snapshot->flags);
        const uint8_t pat_index = guest ? guest->patIndex() : 0;
        auto candidate = candidateFor(*snapshot, guest, cache);
        const ept::LookupInput input(guestPhysical, access, *snapshot->ept,
                                     false, snapshot->cr0.cd);
        ept::walkTiming(
            *snapshot->port, req->requestorId(), input, snapshot->physicalBits,
            snapshot->eptCapabilities,
            [self = shared_from_this()] {
                return !self->squashed() &&
                    self->snapshot->generation == self->cache.generation(
                        self->snapshot->tag.thread);
            },
            [self = shared_from_this(), input, pat_index,
             candidate](const ept::WalkResult &result) {
                self->complete(self->mmu.finishEpt(self->req, *self->snapshot,
                                                   self->cache, input, result,
                                                   pat_index, candidate));
            });
    }

    void complete(const Fault &fault)
    {
        assert(!completed);
        completed = true;
        if (!fault) {
            ++cache.combinedStats.timingCompletions;
            cache.combinedStats.timingTicks += curTick() - start;
        }
        translation->finish(fault, req, tc, snapshot->originalMode);
    }

    void finish(const Fault &fault, const Walker::Result *result) override
    {
        assert(!completed);
        if (fault == NoFault) {
            assert(result);
            if (squashed() ||
                result->generation != cache.generation(snapshot->tag.thread)) {
                complete(std::make_shared<ReExec>());
            } else {
                cache.insert(result->entry.vaddr, result->entry, result->context);
                if (result->entry.uncacheable && !snapshot->ept) {
                    req->setFlags(Request::UNCACHEABLE |
                                  Request::STRICT_ORDER);
                }
                startFinal(result->entry.paddr |
                               (snapshot->linearAddress &
                                mask(result->entry.logBytes)),
                           &result->entry);
            }
        } else {
            complete(fault);
        }
    }
};

namespace
{

Cycles
localMsrAccess(bool read, uint32_t address, ThreadContext *tc, PacketPtr pkt)
{
    if (read) {
        uint64_t value;
        panic_if(!readMsr(tc, address, value), "Invalid checked RDMSR");
        RegVal data = htole(value);
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->setData((uint8_t *)&data);
    } else {
        RegVal data = 0;
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->writeData((uint8_t *)&data);
        panic_if(!writeMsr(tc, address, letoh(data)), "Invalid checked WRMSR");
    }
    return Cycles(1);
}

Cycles
localMiscRegAccess(bool read, RegIndex regNum,
                   ThreadContext *tc, PacketPtr pkt)
{
    if (read) {
        RegVal data = htole(tc->readMiscReg(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->setData((uint8_t *)&data);
    } else {
        RegVal data = htole(tc->readMiscRegNoEffect(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->writeData((uint8_t *)&data);
        tc->setMiscReg(regNum, letoh(data));
    }
    return Cycles(1);
}

} // anonymous namespace

Fault
MMU::translateInt(bool read, RequestPtr req, ThreadContext *tc)
{
    DPRINTF(TLB, "Addresses references internal memory.\n");
    Addr vaddr = req->getVaddr();
    Addr prefix = (vaddr >> 3) & IntAddrPrefixMask;
    if (prefix == IntAddrPrefixCPUID) {
        panic("CPUID memory space not yet implemented!\n");
    } else if (prefix == IntAddrPrefixMSR) {
        vaddr = (vaddr >> 3) & ~IntAddrPrefixMask;

        auto *isa = dynamic_cast<ISA *>(tc->getIsaPtr());
        if (isa) {
            auto &vmx = isa->vmxState();
            const uint32_t msr = static_cast<uint32_t>(vaddr);
            if ((read && vmx.rdmsrCausesExit(tc, msr)) ||
                    (!read && vmx.wrmsrCausesExit(tc, msr))) {
                return vmx.msrExitFault(read, msr);
            }
        }

        RegIndex regNum;
        if (!msrAddrToIndex(regNum, vaddr))
            return std::make_shared<GeneralProtection>(0);
        if (!read && !msrWritable(tc, vaddr)) {
            return std::make_shared<GeneralProtection>(0);
        }

        req->setPaddr(req->getVaddr());
        req->setLocalAccessor([read, address = uint32_t(vaddr)](
                                  ThreadContext *tc, PacketPtr pkt) {
            return localMsrAccess(read, address, tc, pkt);
        });

        return NoFault;
    } else if (prefix == IntAddrPrefixIO) {
        // TODO If CPL > IOPL or in virtual mode, check the I/O permission
        // bitmap in the TSS.

        Addr IOPort = vaddr & ~IntAddrPrefixMask;
        // Make sure the address fits in the expected 16 bit IO address
        // space.
        assert(!(IOPort & ~0xFFFF));
        auto *isa = dynamic_cast<ISA *>(tc->getIsaPtr());
        if (isa) {
            auto &vmx = isa->vmxState();
            if (vmx.ioInstructionCausesExit(tc,
                        static_cast<uint16_t>(IOPort), req->getSize())) {
                return vmx.ioExitFault(read,
                        static_cast<uint16_t>(IOPort), req->getSize());
            }
        }
        if (IOPort == 0xCF8 && req->getSize() == 4) {
            req->setPaddr(req->getVaddr());
            req->setLocalAccessor(
                [read](ThreadContext *tc, PacketPtr pkt)
                {
                    return localMiscRegAccess(
                            read, misc_reg::PciConfigAddress, tc, pkt);
                }
            );
        } else if ((IOPort & ~mask(2)) == 0xCFC) {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            Addr configAddress =
                tc->readMiscRegNoEffect(misc_reg::PciConfigAddress);
            if (bits(configAddress, 31, 31)) {
                req->setPaddr(PhysAddrPrefixPciConfig |
                        mbits(configAddress, 30, 2) |
                        (IOPort & mask(2)));
            } else {
                req->setPaddr(PhysAddrPrefixIO | IOPort);
            }
        } else {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(PhysAddrPrefixIO | IOPort);
        }
        return NoFault;
    } else {
        panic("Access to unrecognized internal address space %#x.\n",
                prefix);
    }
}

Fault
MMU::finalizePhysical(const RequestPtr &req,
                      ThreadContext *tc, BaseMMU::Mode mode) const
{
    return finalizePhysical(req, mode,
        tc->readMiscRegNoEffect(misc_reg::ApicBase), tc->contextId());
}

Fault
MMU::finalizePhysical(const RequestPtr &req, Mode mode,
                      LocalApicBase localApicBase, ContextID thread) const
{
    Addr paddr = req->getPaddr();

    if (m5opRange.contains(paddr)) {
        req->setFlags(Request::STRICT_ORDER);
        uint8_t func;
        pseudo_inst::decodeAddrOffset(paddr - m5opRange.start(), func);
        req->setLocalAccessor(
            [func, mode](ThreadContext *tc, PacketPtr pkt) -> Cycles
            {
                uint64_t ret;
                pseudo_inst::pseudoInst<X86PseudoInstABI, true>(tc, func, ret);
                if (mode == BaseMMU::Read)
                    pkt->setLE(ret);
                return Cycles(1);
            }
        );
    } else if (FullSystem) {
        // Check for an access to the local APIC
        AddrRange apicRange(localApicBase.base * PageBytes,
                            (localApicBase.base + 1) * PageBytes);

        if (apicRange.contains(paddr)) {
            // The Intel developer's manuals say the below restrictions apply,
            // but the linux kernel, because of a compiler optimization, breaks
            // them.
            /*
            // Check alignment
            if (paddr & ((32/8) - 1))
                return new GeneralProtection(0);
            // Check access size
            if (req->getSize() != (32/8))
                return new GeneralProtection(0);
            */
            // Force the access to be uncacheable.
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(x86LocalAPICAddress(thread,
                                              paddr - apicRange.start()));
        }
    }

    return NoFault;
}

Fault
MMU::translate(const RequestPtr &req,
        ThreadContext *tc, BaseMMU::Translation *translation,
        BaseMMU::Mode mode, bool &delayedResponse, bool &completedInline,
        bool timing, bool functional, TLB &cache)
{
    const auto completionMode = mode;
    if (req->isCacheClean())
        mode = BaseMMU::Read;
    Request::Flags flags = req->getFlags();
    int seg = flags & SegmentFlagMask;
    bool storeCheck = flags & Request::READ_MODIFY_WRITE;

    delayedResponse = false;
    completedInline = false;

    if (flags & Request::PHYSICAL) {
        req->setPaddr(req->getVaddr());
        return finalizePhysical(req, tc, mode);
    }

    // If this is true, we're dealing with a request to a non-memory address
    // space.
    if (seg == segment_idx::Ms) {
        return translateInt(mode == BaseMMU::Read, req, tc);
    }

    Addr vaddr = req->getVaddr();
    DPRINTF(TLB, "Translating vaddr %#x.\n", vaddr);
    const bool ept_active = FullSystem && static_cast<ISA *>(tc->getIsaPtr())
                                              ->vmxState()
                                              .eptConfig()
                                              .has_value();
    const TlbEntry *final_guest = nullptr;

    // Port proxies (debugger, pseudo-instructions, workload loaders) name
    // linear addresses on the simulator's behalf. Like the original
    // functional walk, they translate as a supervisor read without segment
    // checks or A/D side effects. CPU-originated functional requests keep
    // the architectural checks of the access they model.
    const bool debugAccess =
        functional && req->requestorId() == Request::funcRequestorId;

    HandyM5Reg m5Reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);

    const Addr logAddrSize = (flags >> AddrSizeFlagShift) & AddrSizeFlagMask;
    const int addrSize = 8 << logAddrSize;
    const Addr addrMask = mask(addrSize);

    if (m5Reg.mode == LongMode) {
        const auto canonical = [](Addr address) {
            return (address >> 47) == 0 || (address >> 47) == mask(17);
        };
        const Addr last = vaddr + req->getSize() - 1;
        if (!canonical(vaddr) || !canonical(last) || last < vaddr) {
            if (seg == segment_idx::Ss && mode != BaseMMU::Execute)
                return std::make_shared<StackFault>(0);
            return std::make_shared<GeneralProtection>(0);
        }
    }

    // If protected mode has been enabled...
    if (m5Reg.prot) {
        DPRINTF(TLB, "In protected mode.\n");
        // If we're not in 64-bit mode, do protection/limit checks
        if (m5Reg.mode != LongMode && !debugAccess) {
            DPRINTF(TLB, "Not in long mode. Checking segment protection.\n");

            // CPUs won't know to use CS when building fetch requests, so we
            // need to override the value of "seg" here if this is a fetch.
            if (mode == BaseMMU::Execute)
                seg = segment_idx::Cs;

            SegAttr attr = tc->readMiscRegNoEffect(misc_reg::segAttr(seg));
            // Check for an unusable segment.
            if (attr.unusable) {
                DPRINTF(TLB, "Unusable segment.\n");
                return std::make_shared<GeneralProtection>(0);
            }
            bool expandDown = false;
            if (seg >= segment_idx::Es && seg <= segment_idx::Hs) {
                if (!attr.writable && (mode == BaseMMU::Write || storeCheck)) {
                    DPRINTF(TLB, "Tried to write to unwritable segment.\n");
                    return std::make_shared<GeneralProtection>(0);
                }
                if (!attr.readable && mode == BaseMMU::Read) {
                    DPRINTF(TLB, "Tried to read from unreadble segment.\n");
                    return std::make_shared<GeneralProtection>(0);
                }
                expandDown = attr.expandDown;

            }
            Addr base = tc->readMiscRegNoEffect(misc_reg::segBase(seg));
            Addr limit = tc->readMiscRegNoEffect(misc_reg::segLimit(seg));
            Addr offset;
            if (mode == BaseMMU::Execute)
                offset = vaddr - base;
            else
                offset = (vaddr - base) & addrMask;
            Addr endOffset = offset + req->getSize() - 1;
            if (expandDown) {
                DPRINTF(TLB, "Checking an expand down segment.\n");
                warn_once("Expand down segments are untested.\n");
                if (offset <= limit || endOffset <= limit)
                    return std::make_shared<GeneralProtection>(0);
            } else {
                if (offset > limit || endOffset > limit) {
                    DPRINTF(TLB, "Segment limit check failed, "
                            "offset = %#x limit = %#x.\n", offset, limit);
                    return std::make_shared<GeneralProtection>(0);
                }
            }
        }
        const Addr linearMask =
            m5Reg.submode != SixtyFourBitMode && addrSize != 64 ?
            mask(32) : ~Addr(0);
        vaddr &= linearMask;
        // CR2 receives the linear address, which is 32 bits outside 64-bit
        // mode (SDM 5.1, 5.7), including after segment-offset wraparound.
        const Addr faultAddr = mode == BaseMMU::Execute && req->hasPC() ?
            paging::faultAddress(vaddr, req->getSize(),
                                 req->getPC() & linearMask) : vaddr;
        // If paging is enabled, do the translation.
        if (m5Reg.paging) {
            DPRINTF(TLB, "Paging enabled.\n");
            // The vaddr already has the segment base applied.

            CR4 cr4 = tc->readMiscRegNoEffect(misc_reg::Cr4);
            Addr pageAlignedVaddr = vaddr & (~mask(X86ISA::PageShift));
            CR3 cr3 = tc->readMiscRegNoEffect(misc_reg::Cr3);
            TlbContext context{tc->contextId(), FullSystem ?
                (cr4.pcide ? uint64_t(cr3.pcid) : 0) :
                tc->getProcessPtr()->pTable->pid()};

            if (ept_active && !functional && cache.combined.enabled()) {
                const auto &ept = static_cast<ISA *>(tc->getIsaPtr())
                                      ->vmxState()
                                      .eptConfig();
                const auto tag = combinedTag(context, cr3, cr4, m5Reg, *ept);
                const auto *hit =
                    cache.combined.lookup(tag, vaddr, req->getSize());
                if (hit) {
                    const CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
                    const bool user = m5Reg.cpl == 3 && !(flags & CPL0FlagBit);
                    const auto access = finalAccess(mode, flags);
                    const bool denied =
                        (user && !hit->user) ||
                        (access == EptAccess::Write && !hit->writable &&
                         (user || cr0.wp)) ||
                        (mode == BaseMMU::Execute && hit->noExec) ||
                        !(hit->eptPermissions & (1U << unsigned(access)));
                    const bool needs_dirty =
                        (mode == BaseMMU::Write && !hit->guestDirty) ||
                        (access == EptAccess::Write && !hit->eptDirtyReady);
                    if (denied) {
                        ++cache.combinedStats.permissionFallbacks;
                    } else if (needs_dirty) {
                        ++cache.combinedStats.dirtyFallbacks;
                    } else {
                        cache.combinedStats.hit(mode);
                        // The slot can be evicted before a delayed hit
                        // returns.
                        const Addr hpa =
                            hit->hostBase | (vaddr & mask(hit->shift));
                        const auto pat =
                            tc->readMiscRegNoEffect(misc_reg::Pat);
                        const auto apic =
                            tc->readMiscRegNoEffect(misc_reg::ApicBase);
                        auto finish_hit = [=, this, index = hit->patIndex,
                                           type = hit->eptType,
                                           ignore = hit->ignorePat] {
                            req->setPaddr(hpa);
                            applyFinalType(req, cr0, pat, index, type, ignore,
                                           access);
                            return finalizePhysical(req, mode, apic,
                                                    context.thread);
                        };
                        if (!timing || !cache.combinedHitLatency) {
                            return finish_hit();
                        }

                        const auto generation =
                            cache.generation(context.thread);
                        auto respond = [=, &cache, this] {
                            Fault fault;
                            if (translation->squashed() ||
                                cache.generation(context.thread) !=
                                    generation) {
                                fault = std::make_shared<ReExec>();
                            } else {
                                fault = finish_hit();
                            }
                            translation->finish(fault, req, tc,
                                                completionMode);
                            if (--pendingWalks == 0) {
                                signalDrainDone();
                            }
                        };
                        ++pendingWalks;
                        delayedResponse = true;
                        schedule(new EventFunctionWrapper(
                                     respond, name() + ".combinedHit", true),
                                 curTick() + tc->getCpuPtr()->cyclesToTicks(
                                                 cache.combinedHitLatency));
                        return NoFault;
                    }
                } else {
                    ++cache.combinedStats.misses;
                }
            }

            TlbEntry functionalEntry;
            TlbEntry *entry = functional ? nullptr :
                cache.lookup(pageAlignedVaddr, true, context);

            if (!functional)
                cache.recordAccess(mode);
            if (FullSystem && entry && mode == BaseMMU::Write &&
                !entry->dirty) {
                // A write the cached rights deny faults from this entry
                // (SDM 5.10.2.3) and must not set D, so keep the entry.
                const bool user = m5Reg.cpl == 3 && !(flags & CPL0FlagBit);
                const CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
                const bool denied = (user && !entry->user) ||
                    (!entry->writable && (user || cr0.wp));
                if (!denied) {
                    // Rewalk to locate the current leaf; do not retain a
                    // pointer to a descriptor that software may have
                    // replaced. This is cache replacement, not an
                    // architectural invalidation.
                    cache.evict(*entry);
                    entry = nullptr;
                }
            }
            if (!entry) {
                DPRINTF(TLB, "Handling a TLB miss for "
                        "address %#x at pc %#x.\n",
                        vaddr, tc->pcState().instAddr());
                if (!functional)
                    cache.recordMiss(mode);
                TranslationContextPtr snapshot;
                if (FullSystem)
                    snapshot = captureContext(req, tc, cache,
                        debugAccess ? BaseMMU::Read : mode, completionMode,
                        vaddr, faultAddr, debugAccess);
                if (FullSystem && functional) {
                    Fault fault = cache.getWalker()->startFunctional(snapshot,
                        functionalEntry);
                    if (fault != NoFault)
                        return fault;
                    return finishWalk(req, *snapshot, functionalEntry, cache,
                                      true);
                } else if (FullSystem) {
                    Walker::Result result;
                    std::shared_ptr<Walker::Completion> continuation;
                    if (timing) {
                        continuation = std::make_shared<WalkContinuation>(
                            *this, cache, req, tc, translation, snapshot);
                    }
                    Fault fault = cache.getWalker()->start(
                        snapshot, std::move(continuation), &result);
                    if (fault != NoFault)
                        return fault;
                    if (timing) {
                        // This gets ignored in atomic mode.
                        delayedResponse = true;
                        return fault;
                    }
                    if (result.generation !=
                            cache.generation(snapshot->tag.thread))
                        return std::make_shared<ReExec>();
                    // Complete from the walk result, as the timing path does;
                    // the cache may hold another page size for this range.
                    cache.insert(result.entry.vaddr, result.entry, result.context);
                    return finishWalk(req, *snapshot, result.entry, cache,
                                      false);
                } else {
                    Process *p = tc->getProcessPtr();
                    const EmulationPageTable::Entry *pte =
                        p->pTable->lookup(vaddr);
                    if (!pte) {
                        return std::make_shared<PageFault>(vaddr, true, mode,
                                                           true, false);
                    } else {
                        Addr alignedVaddr = p->pTable->pageAlign(vaddr);
                        DPRINTF(TLB, "Mapping %#x to %#x\n", alignedVaddr,
                                pte->paddr);
                        entry = cache.insert(alignedVaddr, TlbEntry(
                                p->pTable->pid(), alignedVaddr, pte->paddr,
                                pte->flags & EmulationPageTable::Uncacheable,
                                pte->flags & EmulationPageTable::ReadOnly),
                                context);
                    }
                    DPRINTF(TLB, "Miss was serviced.\n");
                }
            }

            DPRINTF(TLB, "Entry found with paddr %#x, "
                    "doing protection checks.\n", entry->paddr);
            // Do paging protection checks.
            bool inUser = m5Reg.cpl == 3 && !(flags & CPL0FlagBit);
            CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
            bool badWrite = (!entry->writable && (inUser || cr0.wp));
            if ((inUser && !entry->user) ||
                (mode == BaseMMU::Write && badWrite) ||
                (mode == BaseMMU::Execute && entry->noExec)) {
                // The walker validated presence and reserved bits before
                // making this entry available.
                auto faultMode = mode;
                if (mode == BaseMMU::Read && storeCheck)
                    faultMode = BaseMMU::Write;
                if (mode == BaseMMU::Execute &&
                    !(cr4.pae &&
                      Efer(tc->readMiscRegNoEffect(misc_reg::Efer)).nxe))
                    faultMode = BaseMMU::Read;
                return std::make_shared<PageFault>(faultAddr, true, faultMode, inUser,
                                                   false);
            }
            if (storeCheck && badWrite) {
                // This would fault if this were a write, so return a page
                // fault that reflects that happening.
                return std::make_shared<PageFault>(
                    vaddr, true, BaseMMU::Write, inUser, false);
            }

            Addr paddr = entry->paddr | (vaddr & mask(entry->logBytes));
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, paddr);
            req->setPaddr(paddr);
            final_guest = entry;
            if (entry->uncacheable && !ept_active) {
                req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            }
        } else {
            //Use the address which already has segmentation applied.
            DPRINTF(TLB, "Paging disabled.\n");
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
            req->setPaddr(vaddr);
        }
    } else {
        // Real mode
        DPRINTF(TLB, "In real mode.\n");
        DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
        req->setPaddr(vaddr);
    }

    if (ept_active) {
        auto snapshot = captureContext(req, tc, cache, mode, completionMode,
                                       vaddr, vaddr, debugAccess);
        if (timing) {
            auto continuation = std::make_shared<WalkContinuation>(
                *this, cache, req, tc, translation, snapshot);
            continuation->startFinal(req->getPaddr(), final_guest);
            completedInline = continuation->done();
            delayedResponse = !completedInline;
            return NoFault;
        }
        return resolveEpt(req, *snapshot, cache, functional, final_guest);
    }
    return finalizePhysical(req, tc, mode);
}

Fault
MMU::finishWalk(const RequestPtr &req, const TranslationContext &context,
                const TlbEntry &entry, TLB &cache, bool functional)
{
    // The walker returns an explicit stage-one result with permissions
    // checked against its captured context. Completion never re-enters
    // translate() with a null asynchronous continuation.
    req->setPaddr(entry.paddr | (context.linearAddress & mask(entry.logBytes)));
    if (entry.uncacheable && !context.ept) {
        req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
    }
    return resolveEpt(req, context, cache, functional, &entry);
}

Fault
MMU::resolveEpt(const RequestPtr &req, const TranslationContext &context,
                TLB &cache, bool functional, const TlbEntry *guest)
{
    if (!context.ept) {
        return finalizePhysical(req, context.accessMode, context.apicBase,
                                context.tag.thread);
    }
    const auto access = finalAccess(context.accessMode, context.flags);
    const ept::LookupInput input(req->getPaddr(), access, *context.ept, false,
                                 context.cr0.cd);
    const uint8_t pat_index = guest ? guest->patIndex() : 0;
    auto candidate =
        functional ? std::nullopt : candidateFor(context, guest, cache);
    const auto result =
        functional
            ? ept::walkFunctional(*context.port, req->requestorId(), input,
                                  context.physicalBits,
                                  context.eptCapabilities)
            : ept::walkAtomic(*context.port, req->requestorId(), input,
                              context.physicalBits, context.eptCapabilities)
                  .result;
    return finishEpt(req, context, cache, input, result, pat_index, candidate);
}

Fault
MMU::finishEpt(const RequestPtr &req, const TranslationContext &context,
               TLB &cache, const ept::LookupInput &input,
               const ept::WalkResult &result, uint8_t pat_index,
               std::optional<CombinedEntry> candidate)
{
    if (candidate &&
        context.generation != cache.generation(context.tag.thread)) {
        return std::make_shared<ReExec>();
    }
    if (result.status != ept::WalkStatus::Mapping) {
        return ept::lookupFault(result, input, context.linearAddress, true);
    }

    req->setPaddr(result.hostPhysical);
    applyFinalType(req, context.cr0, context.pat, pat_index, result.memoryType,
                   result.ignorePat, input.access);
    Fault fault = finalizePhysical(req, context.accessMode, context.apicBase,
                                   context.tag.thread);
    if (!fault && candidate) {
        auto &entry = *candidate;
        entry.shift = std::min<unsigned>(entry.shift, result.pageShift);
        const Addr base_mask = ~mask(entry.shift);
        entry.linearBase = context.linearAddress & base_mask;
        entry.guestBase = input.guestPhysical & base_mask;
        entry.hostBase = result.hostPhysical & base_mask;
        entry.eptPermissions = result.permissions;
        entry.eptType = result.memoryType;
        entry.ignorePat = result.ignorePat;
        // result.entry is the word observed before the walker's own update.
        entry.eptDirtyReady = !input.config.accessedDirty ||
                              bits(result.entry, 9) ||
                              input.access == EptAccess::Write;
        if (cache.combined.insert(entry)) {
            ++cache.combinedStats.evictions;
        }
    }
    return fault;
}

Fault
MMU::translateAtomic(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Mode mode)
{
    TLB &cache = *static_cast<TLB *>(getTlb(mode));
    bool delayedResponse;
    bool completedInline;
    return translate(req, tc, nullptr, mode, delayedResponse,
                     completedInline, false, false, cache);
}

Fault
MMU::translateFunctional(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Mode mode)
{
    TLB &cache = *static_cast<TLB *>(getTlb(mode));
    if (FullSystem) {
        bool delayed;
        bool completedInline;
        return translate(req, tc, nullptr, mode, delayed, completedInline,
                         false, true, cache);
    }
    if (req->isCacheClean())
        mode = BaseMMU::Read;
    const Addr vaddr = req->getVaddr();
    Addr paddr = 0;
    {
        Process *process = tc->getProcessPtr();
        const auto *pte = process->pTable->lookup(vaddr);

        if (!pte && mode != BaseMMU::Execute) {
            // Check if we just need to grow the stack.
            if (process->fixupFault(vaddr)) {
                // If we did, lookup the entry for the new page.
                pte = process->pTable->lookup(vaddr);
            }
        }

        if (!pte)
            return std::make_shared<PageFault>(vaddr, true, mode, true, false);

        paddr = pte->paddr | process->pTable->pageOffset(vaddr);
    }
    DPRINTF(TLB, "Translated (functional) %#x -> %#x.\n", vaddr, paddr);
    req->setPaddr(paddr);
    return NoFault;
}

void
MMU::translateTiming(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    bool delayedResponse;
    bool completedInline;
    assert(translation);
    Fault fault =
        translate(req, tc, translation, mode, delayedResponse,
                  completedInline, true, false,
                  *static_cast<TLB *>(getTlb(mode)));
    if (completedInline)
        return;
    if (!delayedResponse)
        translation->finish(fault, req, tc, mode);
    else
        translation->markDelayed();
}

} // namespace X86ISA
} // namespace gem5
