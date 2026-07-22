/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
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

#ifndef __ARCH_X86_VMX_UTILS_HH__
#define __ARCH_X86_VMX_UTILS_HH__

#include <cstdint>

#include "base/bitfield.hh"

namespace gem5
{
namespace X86ISA
{
namespace vmx
{

// CR0 bits loaded by VM entry and VM exit. ET, CD, NW, and reserved bits
// retain their live values (Intel SDM Vol. 3C, 29.3.2.1 and 30.5.1).
inline constexpr uint64_t LoadedCr0Bits =
    (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) |
    (1ull << 5) | (1ull << 16) | (1ull << 18) | (1ull << 31);

inline constexpr uint64_t
controlCapabilityMsr(uint32_t requiredOne, uint32_t allowedOne)
{
    return static_cast<uint64_t>(requiredOne) |
        (static_cast<uint64_t>(allowedOne) << 32);
}

inline constexpr bool
controlsAllowed(uint32_t controls, uint64_t capability)
{
    const uint32_t requiredOne = bits(capability, 31, 0);
    const uint32_t allowedOne = bits(capability, 63, 32);
    return (controls & requiredOne) == requiredOne &&
        (controls & ~allowedOne) == 0;
}

inline constexpr bool
fixedBitsAllowed(uint64_t value, uint64_t fixed0, uint64_t fixed1,
        uint64_t ignored = 0)
{
    const uint64_t required = fixed0 & ~ignored;
    const uint64_t allowed = fixed1 | ignored;
    return (value & required) == required && (value & ~allowed) == 0;
}

inline constexpr bool
validPhysicalAddress(uint64_t value, unsigned physicalAddressBits)
{
    return physicalAddressBits >= 64 ||
        (value & ~mask(physicalAddressBits)) == 0;
}

inline bool
validCr3(uint64_t value, uint64_t cr4, bool ia32e,
        unsigned physicalAddressBits)
{
    // PCID is deliberately not exposed by gem5-vmx. With PCIDE clear, PWT
    // and PCD are the only defined low CR3 bits in non-PAE and IA-32e
    // paging. Legacy PAE requires a 32-byte-aligned PDPTE base.
    const bool pae = bits(cr4, 5);
    const bool pcide = bits(cr4, 17);
    if (pcide) {
        return false;
    }

    const uint64_t base = ia32e || !pae ? value & ~mask(12) :
        value & ~mask(5);
    if (!validPhysicalAddress(base, physicalAddressBits)) {
        return false;
    }

    if (pae && !ia32e) {
        return bits(value, 4, 0) == 0;
    }

    constexpr uint64_t allowedLow = (1ull << 3) | (1ull << 4);
    return (value & mask(12) & ~allowedLow) == 0;
}

inline constexpr uint64_t
mergeLoadedCr0(uint64_t live, uint64_t vmcsValue)
{
    return (live & ~LoadedCr0Bits) | (vmcsValue & LoadedCr0Bits);
}

inline constexpr bool
cltsCausesExit(uint64_t guestHostMask, uint64_t readShadow)
{
    return bits(guestHostMask, 3) && bits(readShadow, 3);
}

inline constexpr bool
lmswCausesExit(uint64_t guestHostMask, uint64_t readShadow,
        uint16_t source)
{
    // LMSW cannot clear CR0.PE. Consequently bit 0 exits only for a source
    // one against a shadow zero; bits 3:1 are ordinary masked comparisons.
    const bool peExit = bits(guestHostMask, 0) && bits(source, 0) &&
        !bits(readShadow, 0);
    const bool otherExit =
        ((source ^ readShadow) & guestHostMask & 0xe) != 0;
    return peExit || otherExit;
}

} // namespace vmx
} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_VMX_UTILS_HH__
