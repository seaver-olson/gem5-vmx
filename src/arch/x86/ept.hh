/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __ARCH_X86_EPT_HH__
#define __ARCH_X86_EPT_HH__

#include <cstdint>
#include <optional>

#include "base/bitfield.hh"
#include "base/types.hh"

namespace gem5::X86ISA
{

// Decoded once at VM entry. A translation captures a copy so later VMCS
// writes cannot change the root or interpretation of an in-flight walk.
struct EptConfig
{
    uint64_t eptp;
    Addr root;
    uint8_t walkLength;
    uint8_t memoryType;
    bool accessedDirty;
};

namespace ept
{

constexpr uint32_t EnableEptSecondaryControl = 1u << 1;

// Intel SDM Vol. 3C, EPTP format and VM-entry execution-control checks.
// The capability argument is IA32_VMX_EPT_VPID_CAP. This parser does not
// enable EPT or perform any guest-physical translation.
inline std::optional<EptConfig>
decodeEptp(uint64_t eptp, uint64_t capability, unsigned physicalBits)
{
    if (physicalBits < 32 || physicalBits > 52 || bits(eptp, 11, 7) ||
            (eptp & ~mask(physicalBits) & ~mask(12))) {
        return std::nullopt;
    }

    const uint8_t memoryType = bits(eptp, 2, 0);
    if ((memoryType == 0 && !bits(capability, 8)) ||
            (memoryType == 6 && !bits(capability, 14)) ||
            (memoryType != 0 && memoryType != 6)) {
        return std::nullopt;
    }

    const uint8_t walkLength = bits(eptp, 5, 3) + 1;
    if ((walkLength == 4 && !bits(capability, 6)) ||
            (walkLength == 5 && !bits(capability, 7)) ||
            (walkLength != 4 && walkLength != 5)) {
        return std::nullopt;
    }

    const bool accessedDirty = bits(eptp, 6);
    if (accessedDirty && !bits(capability, 21)) {
        return std::nullopt;
    }

    return EptConfig{eptp, eptp & ~mask(12), walkLength, memoryType,
                     accessedDirty};
}

} // namespace ept
} // namespace gem5::X86ISA

#endif // __ARCH_X86_EPT_HH__
