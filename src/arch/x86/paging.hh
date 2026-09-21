/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __ARCH_X86_PAGING_HH__
#define __ARCH_X86_PAGING_HH__

#include <cstdint>
#include <cstring>

#include "base/amo.hh"
#include "base/bitfield.hh"
#include "sim/byteswap.hh"

namespace gem5::X86ISA::paging
{

inline bool
validPaePdpte(uint64_t value, unsigned physicalBits)
{
    // NX is reserved here even with NXE enabled; permissions begin at PDE.
    return !(value & 1) || !(value & (0x1e6ULL | ~mask(physicalBits)));
}

inline bool
reloadPaePdpte(unsigned reg, uint64_t oldCr0, uint64_t oldCr4,
               uint64_t efer, uint64_t value)
{
    const auto cr0 = reg == 0 ? value : oldCr0;
    const auto cr4 = reg == 4 ? value : oldCr4;
    if (!bits(cr0, 31) || !bits(cr4, 5) || bits(efer, 8))
        return false;
    return reg == 3 ||
        (reg == 0 && ((oldCr0 ^ cr0) & ((1ULL << 31) | (3ULL << 29)))) ||
        (reg == 4 && ((oldCr4 ^ cr4) &
                     ((1ULL << 4) | (1ULL << 5) | (1ULL << 7) | (1ULL << 20))));
}

// O3 translates aligned fetch buffers. Report the instruction's first byte,
// or the first byte in this buffer when an instruction crosses its boundary.
inline uint64_t
faultAddress(uint64_t address, uint64_t size, uint64_t pc)
{
    return pc >= address && pc - address < size ? pc : address;
}

inline bool
reservedAddress(uint64_t descriptor, unsigned physicalBits)
{
    return (descriptor & (mask(52) & ~mask(physicalBits))) != 0;
}

// Descriptor replacement must never be overwritten by an A/D update. Compare
// the entire observed little-endian descriptor, then OR only the requested bits.
// A failed comparison leaves memory untouched; the returned old value tells
// the walker to restart. memcpy avoids host alignment/aliasing assumptions.
template <class T>
class ConditionalUpdate : public AtomicOpFunctor
{
  private:
    const T observed;
    const T bitsToSet;

  public:
    ConditionalUpdate(T old, T bits) : observed(old), bitsToSet(bits) {}
    AtomicOpFunctor *clone() override { return new ConditionalUpdate(*this); }
    void operator()(uint8_t *bytes) override
    {
        T current;
        std::memcpy(&current, bytes, sizeof(T));
        current = letoh(current);
        if (current == observed) {
            const T updated = htole(T(current | bitsToSet));
            std::memcpy(bytes, &updated, sizeof(T));
        }
    }
};

} // namespace gem5::X86ISA::paging
#endif
