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

// Keep the cause of a second-stage lookup with its GPA. Guest descriptor
// reads/updates and the final access need different EPT-violation metadata.
enum class EptAccess : uint8_t { Read, Write, Execute };
enum class EptOrigin : uint8_t { GuestPageTable, FinalAccess };

struct EptWalkRequest
{
    EptConfig config;
    Addr guestPhysical;
    EptAccess access;
    EptOrigin origin;
    unsigned hostPhysicalBits;
};

namespace ept
{

constexpr uint32_t EnableEptSecondaryControl = 1u << 1;
constexpr uint32_t ActivateSecondaryControls = 1u << 31;

// These VMCS components exist only if their enabling control can be 1.
// Capability MSRs put allowed-one control bits in the high dword.
inline constexpr bool
secondaryControlsAvailable(uint64_t primaryCapability)
{
    return (primaryCapability >> 32) & ActivateSecondaryControls;
}

inline constexpr bool
eptPointerAvailable(uint64_t primaryCapability,
                    uint64_t secondaryCapability)
{
    return secondaryControlsAvailable(primaryCapability) &&
        ((secondaryCapability >> 32) & EnableEptSecondaryControl);
}

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

// The address-planning half of a second-stage walk. It does not fetch an EPT
// entry or grant access. EPT table bases and entry addresses are host-physical;
// a continuation can use them on the existing PagingPort without queuing
// behind its guest-page-walk parent.
class WalkPlan
{
  private:
    EptWalkRequest request;

    explicit WalkPlan(const EptWalkRequest &input) : request(input) {}

  public:
    static std::optional<WalkPlan>
    create(const EptWalkRequest &input)
    {
        const unsigned levels = input.config.walkLength;
        if ((levels != 4 && levels != 5) ||
                input.hostPhysicalBits < 32 ||
                input.hostPhysicalBits > 52 ||
                (input.guestPhysical & ~mask(12 + 9 * levels)) ||
                (input.config.root & mask(12)) ||
                (input.config.root & ~mask(input.hostPhysicalBits))) {
            return std::nullopt;
        }
        return WalkPlan(input);
    }

    const EptWalkRequest &input() const { return request; }

    std::optional<Addr>
    entryAddress(Addr tableBase, unsigned level) const
    {
        if (level == 0 || level > request.config.walkLength ||
                (tableBase & mask(12)) ||
                (tableBase & ~mask(request.hostPhysicalBits))) {
            return std::nullopt;
        }
        const unsigned shift = 12 + 9 * (level - 1);
        const Addr index = (request.guestPhysical >> shift) & mask(9);
        return tableBase + index * sizeof(uint64_t);
    }

    std::optional<Addr>
    rootEntryAddress() const
    {
        return entryAddress(request.config.root, request.config.walkLength);
    }
};

} // namespace ept
} // namespace gem5::X86ISA

#endif // __ARCH_X86_EPT_HH__
