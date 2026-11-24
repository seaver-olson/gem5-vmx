/* Copyright (c) 2026 Seaver Olson, Loyola University Chicago
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __ARCH_X86_COMBINED_TLB_HH__
#define __ARCH_X86_COMBINED_TLB_HH__

#include <algorithm>
#include <list>
#include <tuple>

#include "base/bitfield.hh"
#include "base/types.hh"

namespace gem5::X86ISA
{

struct CombinedTag
{
    ContextID thread;
    Addr root;
    uint64_t pcid;
    uint64_t eptp;

    bool
    operator==(const CombinedTag &other) const
    {
        return std::tie(thread, root, pcid, eptp) ==
               std::tie(other.thread, other.root, other.pcid, other.eptp);
    }
};

struct CombinedEntry
{
    CombinedTag tag;
    Addr linearBase, guestBase, hostBase;
    unsigned shift;
    bool writable, user, noExec, guestDirty;
    uint8_t patIndex, eptPermissions, eptType;
    bool ignorePat, eptDirtyReady;

    bool
    covers(Addr address, unsigned size) const
    {
        const Addr offset = address - linearBase;
        const Addr last = mask(shift);
        return size && offset <= last && size - 1 <= last - offset;
    }
};

// Most recently used first. Guest replacement and combined replacement
// are independent. Architectural invalidation reaches both structures.
class CombinedTLB
{
  private:
    const unsigned capacity;
    std::list<CombinedEntry> entries;

  public:
    explicit CombinedTLB(unsigned count) : capacity(count) {}
    bool
    enabled() const
    {
        return capacity != 0;
    }

    const CombinedEntry *
    lookup(const CombinedTag &tag, Addr address, unsigned size)
    {
        auto it =
            std::find_if(entries.begin(), entries.end(), [&](const auto &e) {
                return e.tag == tag && e.covers(address, size);
            });
        if (it == entries.end()) {
            return nullptr;
        }
        entries.splice(entries.begin(), entries, it);
        return &*it;
    }

    // Return true only for a capacity eviction.
    bool
    insert(const CombinedEntry &entry)
    {
        if (!enabled()) {
            return false;
        }
        entries.remove_if([&](const auto &e) {
            return e.tag == entry.tag && (e.covers(entry.linearBase, 1) ||
                                          entry.covers(e.linearBase, 1));
        });
        const bool evicted = entries.size() == capacity;
        if (evicted) {
            entries.pop_back();
        }
        entries.push_front(entry);
        return evicted;
    }

    void
    clear()
    {
        entries.clear();
    }

    void
    invalidateThread(ContextID thread)
    {
        entries.remove_if(
            [thread](const auto &e) { return e.tag.thread == thread; });
    }
};

} // namespace gem5::X86ISA

#endif
