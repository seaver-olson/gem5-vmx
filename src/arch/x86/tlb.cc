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

#include "arch/x86/tlb.hh"

#include "arch/x86/mmu.hh"
#include "arch/x86/pagetable_walker.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace X86ISA {

TLB::TLB(const Params &p)
    : BaseTLB(p), configAddress(0), size(p.size),
      tlb(size), lruSeq(0), stats(this)
{
    if (!size)
        fatal("TLBs must have a non-zero size.\n");

    fatal_if(nextLevel(), "The x86 backend does not support multi-level "
                          "TLBs.\n");

    for (int x = 0; x < size; x++) {
        tlb[x].trieHandle = NULL;
        freeList.push_back(&tlb[x]);
    }

    walker = p.walker;
    walker->setTLB(this);
}

void
TLB::recordAccess(BaseMMU::Mode mode)
{
    switch (mode) {
      case BaseMMU::Read: ++stats.rdAccesses; break;
      case BaseMMU::Write: ++stats.wrAccesses; break;
      case BaseMMU::Execute: ++stats.exAccesses; break;
      default: panic("Invalid translation access mode");
    }
}

void
TLB::recordMiss(BaseMMU::Mode mode)
{
    switch (mode) {
      case BaseMMU::Read: ++stats.rdMisses; break;
      case BaseMMU::Write: ++stats.wrMisses; break;
      case BaseMMU::Execute: ++stats.exMisses; break;
      default: panic("Invalid translation access mode");
    }
}

void
TLB::evict(TlbEntry &entry)
{
    assert(entry.trieHandle);
    tries.at(entry.context).remove(entry.trieHandle);
    entry.trieHandle = nullptr;
    freeList.push_back(&entry);
}

void
TLB::evictLRU()
{
    // Find the entry with the lowest (and hence least recently updated)
    // sequence number.

    unsigned lru = 0;
    for (unsigned i = 1; i < size; i++) {
        if (tlb[i].lruSeq < tlb[lru].lruSeq)
            lru = i;
    }

    evict(tlb[lru]);
}

TlbEntry *
TLB::insert(Addr vpn, const TlbEntry &entry, TlbContext context)
{
    auto &trie = tries[context];
    // If somebody beat us to it, just use that existing entry.
    TlbEntry *newEntry = trie.lookup(vpn);
    if (newEntry) {
        assert(newEntry->vaddr == vpn);
        return newEntry;
    }

    if (freeList.empty())
        evictLRU();

    newEntry = freeList.front();
    freeList.pop_front();

    *newEntry = entry;
    newEntry->lruSeq = nextSeq();
    newEntry->vaddr = vpn;
    newEntry->context = context;
    if (FullSystem) {
        newEntry->trieHandle =
        trie.insert(vpn, TlbEntryTrie::MaxBits-entry.logBytes, newEntry);
    }
    else {
        newEntry->trieHandle =
        trie.insert(vpn, TlbEntryTrie::MaxBits, newEntry);
    }
    return newEntry;
}

TlbEntry *
TLB::lookup(Addr va, bool update_lru, TlbContext context)
{
    auto it = tries.find(context);
    TlbEntry *entry = it == tries.end() ? nullptr : it->second.lookup(va);
    if (entry && update_lru)
        entry->lruSeq = nextSeq();
    return entry;
}

void
TLB::flushAll()
{
    ++invalidationGeneration;
    DPRINTF(TLB, "Invalidating all entries.\n");
    for (unsigned i = 0; i < size; i++) {
        if (tlb[i].trieHandle) {
            tries.at(tlb[i].context).remove(tlb[i].trieHandle);
            tlb[i].trieHandle = NULL;
            freeList.push_back(&tlb[i]);
        }
    }
}

void
TLB::setConfigAddress(uint32_t addr)
{
    configAddress = addr;
}

void
TLB::flushNonGlobal()
{
    ++invalidationGeneration;
    DPRINTF(TLB, "Invalidating all non global entries.\n");
    for (unsigned i = 0; i < size; i++) {
        if (tlb[i].trieHandle && !tlb[i].global) {
            tries.at(tlb[i].context).remove(tlb[i].trieHandle);
            tlb[i].trieHandle = NULL;
            freeList.push_back(&tlb[i]);
        }
    }
}

void
TLB::demapPage(Addr va, uint64_t asn)
{
    ++invalidationGeneration;
    // INVLPG also invalidates any global translation for the address.
    for (auto &entry : tlb) {
        if (entry.trieHandle && (entry.context.addressSpace == asn || entry.global) &&
            (va & ~mask(entry.logBytes)) == entry.vaddr) {
            tries.at(entry.context).remove(entry.trieHandle);
            entry.trieHandle = nullptr;
            freeList.push_back(&entry);
        }
    }
}

void
TLB::invalidatePage(Addr va, TlbContext context)
{
    ++invalidationGeneration;
    for (auto &entry : tlb) {
        if (entry.trieHandle && entry.context.thread == context.thread &&
            (entry.context.addressSpace == context.addressSpace || entry.global) &&
            (va & ~mask(entry.logBytes)) == entry.vaddr) {
            tries.at(entry.context).remove(entry.trieHandle);
            entry.trieHandle = nullptr;
            freeList.push_back(&entry);
        }
    }
}

// BaseTLB compatibility entrypoints. CPU requests are coordinated by MMU.
Fault
TLB::translateAtomic(const RequestPtr &req, ThreadContext *tc, BaseMMU::Mode mode)
{
    return static_cast<MMU *>(tc->getMMUPtr())->translateAtomic(req, tc, mode);
}

Fault
TLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Mode mode)
{
    return static_cast<MMU *>(tc->getMMUPtr())->translateFunctional(req, tc, mode);
}

void
TLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
                     BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    static_cast<MMU *>(tc->getMMUPtr())->translateTiming(req, tc, translation, mode);
}

Fault
TLB::finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Mode mode) const
{
    return static_cast<MMU *>(tc->getMMUPtr())->finalizePhysical(req, tc, mode);
}

Walker *
TLB::getWalker()
{
    return walker;
}

TLB::TlbStats::TlbStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(rdAccesses, statistics::units::Count::get(),
               "TLB accesses on read requests"),
      ADD_STAT(wrAccesses, statistics::units::Count::get(),
               "TLB accesses on write requests"),
      ADD_STAT(exAccesses, statistics::units::Count::get(),
               "TLB accesses on execute (instr) requests"),
      ADD_STAT(rdMisses, statistics::units::Count::get(),
               "TLB misses on read requests"),
      ADD_STAT(wrMisses, statistics::units::Count::get(),
               "TLB misses on write requests"),
      ADD_STAT(exMisses, statistics::units::Count::get(),
               "TLB misses on execute (instr) requests")
{
}

void
TLB::serialize(CheckpointOut &cp) const
{
    uint32_t _size = 0;
    SERIALIZE_SCALAR(_size);
    SERIALIZE_SCALAR(lruSeq);
}

void
TLB::unserialize(CheckpointIn &cp)
{
    flushAll();
    uint32_t _size;
    UNSERIALIZE_SCALAR(_size);
    UNSERIALIZE_SCALAR(lruSeq);
    // Translation caches are microarchitectural state. Old checkpoints may
    // contain Entry sections, but no live translation is restored from them.

}

Port *
TLB::getTableWalkerPort()
{
    return &walker->getPort("port");
}

} // namespace X86ISA
} // namespace gem5
