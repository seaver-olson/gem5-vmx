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
#include "base/logging.hh"
#include "mem/request.hh"
#include "sim/byteswap.hh"

namespace gem5::X86ISA::paging
{

using MemoryType = Request::MemoryType;

inline bool
validPatType(uint8_t type)
{
    return type == 0 || type == 1 || (type >= 4 && type <= 7);
}

inline MemoryType
memoryType(uint8_t encoding)
{
    switch (encoding) {
        case 0:
            return MemoryType::Uncacheable;
        case 1:
            return MemoryType::WriteCombining;
        case 4:
            return MemoryType::WriteThrough;
        case 5:
            return MemoryType::WriteProtected;
        case 6:
            return MemoryType::WriteBack;
        default:
            panic("Invalid resolved x86 memory type %u", encoding);
    }
}

// SDM Vol. 3C 29.3.7.2 and Vol. 3A Table 12-7. The EPT type replaces
// the MTRR input; host MTRRs do not participate. UC is not an unconditional
// winner: for example, EPT UC combined with PAT WC produces WC.
inline MemoryType
effectiveEptType(bool cacheDisabled, uint8_t eptType, bool ignorePat,
                 uint8_t patType)
{
    if (cacheDisabled) {
        return MemoryType::Uncacheable;
    }
    if (ignorePat) {
        return memoryType(eptType);
    }
    assert(validPatType(patType));
    assert(eptType == 0 || eptType == 1 || (eptType >= 4 && eptType <= 6));
    constexpr uint8_t combined[7][8] = {
        // PAT: UC WC  -  - WT WP WB UC-
        {0, 1, 0, 0, 0, 0, 0, 0}, // EPT UC
        {0, 1, 0, 0, 0, 0, 1, 1}, // EPT WC
        {},
        {},                       // Reserved EPT encodings
        {0, 1, 0, 0, 4, 5, 4, 0}, // EPT WT
        {0, 1, 0, 0, 4, 5, 5, 1}, // EPT WP
        {0, 1, 0, 0, 4, 5, 6, 0}, // EPT WB
    };
    return memoryType(combined[eptType][patType]);
}

inline uint8_t
patType(uint64_t pat, unsigned index)
{
    assert(index < 8);
    return uint8_t(pat >> (index * 8));
}

// WT uses the SDM-permitted invalidating write-around policy: reads may
// cache, but a write does not allocate and reaches memory. WP also requires
// write invalidation. WC bypasses caches without imposing UC ordering;
// combining buffers are a separate microarchitectural timing choice.
inline void
applyMemoryType(Request &request, MemoryType type, bool write)
{
    request.memoryType(type);
    if (type == MemoryType::Uncacheable ||
        type == MemoryType::WriteCombining ||
        (write && (type == MemoryType::WriteThrough ||
                   type == MemoryType::WriteProtected))) {
        request.setFlags(Request::UNCACHEABLE);
    }
    if (type == MemoryType::Uncacheable) {
        request.setFlags(Request::STRICT_ORDER);
    }
    // Never clear unrelated caller/device constraints for a WB mapping.
}

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
