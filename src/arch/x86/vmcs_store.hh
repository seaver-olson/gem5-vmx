/*
 * Copyright (c) 2026 Seaver Olson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ARCH_X86_VMCS_STORE_HH__
#define __ARCH_X86_VMCS_STORE_HH__

#include <map>
#include <string>
#include <utility>

#include "arch/x86/vmcs.hh"

namespace gem5::X86ISA
{

// System-owned opaque VMCS components, not per-CPU cached copies. Region
// headers still use real memory operands; the component format is unchanged.
class VmcsStore
{
  private:
    struct Record
    {
        Vmcs components;
        ContextID owner = InvalidContextID;
        Record(Addr address, uint32_t revision) : components(address, revision)
        {}
    };
    std::map<Addr, Record> regions;

    static void
    checkOwner(const Record &record, ContextID processor)
    {
        fatal_if(processor == InvalidContextID,
                 "VMCS operation without a logical processor identity");
        // Concurrent activation is invalid software, not a defined VMfail
        // error. Diagnose it instead of fabricating an architectural result.
        fatal_if(record.owner != InvalidContextID && record.owner != processor,
                 "VMCS %#x is active on logical processor %d; VMCLEAR it "
                 "there before using it on logical processor %d",
                 record.components.pointer(), record.owner, processor);
    }

  public:
    Vmcs *
    find(Addr address)
    {
        auto it = regions.find(address);
        return it == regions.end() ? nullptr : &it->second.components;
    }

    bool
    ownedBy(Addr address, ContextID processor) const
    {
        auto it = regions.find(address);
        return it != regions.end() && it->second.owner == processor;
    }

    void
    activate(Addr address, uint32_t revision, ContextID processor)
    {
        auto &record =
            regions.try_emplace(address, address, revision).first->second;
        checkOwner(record, processor);
        record.owner = processor;
        record.components.setActive(true);
    }

    void
    clear(Addr address, uint32_t revision, ContextID processor)
    {
        auto &record =
            regions.try_emplace(address, address, revision).first->second;
        checkOwner(record, processor);
        record.components.clear();
        record.owner = InvalidContextID;
    }

    void
    release(ContextID processor)
    {
        if (processor == InvalidContextID) {
            return;
        }
        for (auto &[address, record] : regions) {
            if (record.owner == processor) {
                record.components.setActive(false);
                record.owner = InvalidContextID;
            }
        }
    }

    void
    restoreRegion(Vmcs components, ContextID processor)
    {
        const Addr address = components.pointer();
        const auto [it, inserted] =
            regions.try_emplace(address, address, components.revisionId());
        fatal_if(!inserted,
                 "Malformed VMCS checkpoint: duplicate region %#x; "
                 "legacy per-CPU copies cannot be reconciled",
                 address);
        it->second.owner = components.active() ? processor : InvalidContextID;
        it->second.components = std::move(components);
    }

    void
    serialize(CheckpointOut &cp) const
    {
        paramOut(cp, "numRegions", regions.size());
        unsigned index = 0;
        for (const auto &[address, record] : regions) {
            Serializable::ScopedCheckpointSection sec(
                cp, "region" + std::to_string(index++));
            record.components.serialize(cp);
            paramOut(cp, "owner", record.owner);
        }
    }

    void
    unserialize(CheckpointIn &cp, int processors)
    {
        regions.clear();
        size_t count;
        paramIn(cp, "numRegions", count);
        for (size_t index = 0; index < count; ++index) {
            Serializable::ScopedCheckpointSection sec(
                cp, "region" + std::to_string(index));
            Vmcs components;
            components.unserialize(cp);
            ContextID owner;
            paramIn(cp, "owner", owner);
            const Addr address = components.pointer();
            fatal_if(address & ~mask(52) ||
                         (address & (Vmcs::VmcsRegionSize - 1)) ||
                         owner < InvalidContextID || owner >= processors ||
                         components.active() != (owner != InvalidContextID),
                     "Malformed shared VMCS checkpoint at %#x", address);
            restoreRegion(std::move(components), owner);
        }
    }
};

} // namespace gem5::X86ISA

#endif
