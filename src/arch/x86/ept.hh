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

enum class EptAccess : uint8_t { Read, Write, Execute };

namespace ept
{

// Inputs for one GPA lookup. The caller supplies an already validated,
// captured configuration; this object owns a copy, not live VMCS state.
// Construction does not validate the address, walk tables or grant access.
struct LookupInput
{
    const Addr guestPhysical;
    const EptAccess access;
    const EptConfig config;

    LookupInput(Addr guestPhysical, EptAccess access, const EptConfig &config)
        : guestPhysical(guestPhysical), access(access), config(config)
    {}
};

// A non-leaf can deny a requested access, but a later entry may still be
// misconfigured. Only a leaf can decide the final permission outcome.
enum class EntryKind : uint8_t { NextTable, Mapping, Violation,
                                 Misconfiguration };

struct EntryResult
{
    EntryKind kind;
    Addr base = 0;
    uint8_t permissions = 0;
    uint8_t pageShift = 0;
    uint8_t memoryType = 0;
};

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

// Decode one entry in a walk, with level 1 denoting an EPT PTE. The caller
// passes the effective permissions from all ancestors (7 at the root) to
// the next level. This covers ordinary R/W/X EPT; controls that alter those
// semantics (MBEC, SPP, paging-write, #VE) are not enabled by this model.
// Absent entries cause violations before their otherwise-ignored bits are
// checked. A denied non-leaf must still be walked to detect a misconfigured
// descendant before reporting the final permission violation.
inline std::optional<EntryResult>
decodeEntry(uint64_t raw, unsigned level, unsigned physicalBits,
            uint64_t capability, uint8_t inheritedPermissions,
            EptAccess access)
{
    if (level < 1 || level > 5 || physicalBits < 32 ||
            physicalBits > 52 || inheritedPermissions > 7 ||
            (access != EptAccess::Read && access != EptAccess::Write &&
             access != EptAccess::Execute)) {
        return std::nullopt;
    }

    if (!(raw & 7)) {
        return EntryResult{EntryKind::Violation};
    }
    if ((!bits(raw, 0) && bits(raw, 1)) ||
            (!bits(raw, 0) && bits(raw, 2) && !bits(capability, 0))) {
        return EntryResult{EntryKind::Misconfiguration};
    }

    const bool large = (level == 2 || level == 3) && bits(raw, 7);
    const bool leaf = level == 1 || large;
    const unsigned pageShift = leaf ? 12 + 9 * (level - 1) : 12;
    if ((!leaf && bits(raw, 7, 3)) ||
            (large && !bits(capability, level == 2 ? 16 : 17)) ||
            (raw & mask(52) & ~mask(physicalBits) & ~mask(pageShift)) ||
            (large && bits(raw, pageShift - 1, 12))) {
        return EntryResult{EntryKind::Misconfiguration};
    }

    const uint8_t memoryType = leaf ? bits(raw, 5, 3) : 0;
    if (leaf && (memoryType == 2 || memoryType == 3 || memoryType == 7)) {
        return EntryResult{EntryKind::Misconfiguration};
    }

    const uint8_t permissions = inheritedPermissions & (raw & 7);
    const Addr base = raw & mask(52) & ~mask(pageShift);
    if (!leaf) {
        return EntryResult{EntryKind::NextTable, base, permissions};
    }

    const unsigned accessBit = access == EptAccess::Read ? 0 :
        access == EptAccess::Write ? 1 : 2;
    const EntryKind kind = permissions & (1u << accessBit) ?
        EntryKind::Mapping : EntryKind::Violation;
    return EntryResult{kind, base, permissions,
                       static_cast<uint8_t>(pageShift), memoryType};
}

// Compute the host-physical address of an EPT entry (level 1 is a PTE).
// The walk length bounds the GPA; the host width bounds the table address.
inline std::optional<Addr>
entryAddress(Addr tableBase, Addr guestPhysical, unsigned level,
             unsigned walkLength, unsigned hostPhysicalBits)
{
    if ((walkLength != 4 && walkLength != 5) ||
            level == 0 || level > walkLength ||
            hostPhysicalBits < 32 || hostPhysicalBits > 52 ||
            (guestPhysical & ~mask(12 + 9 * walkLength)) ||
            (tableBase & mask(12)) ||
            (tableBase & ~mask(hostPhysicalBits))) {
        return std::nullopt;
    }
    const unsigned shift = 12 + 9 * (level - 1);
    const Addr index = (guestPhysical >> shift) & mask(9);
    return tableBase + index * sizeof(uint64_t);
}

// Locate the first entry using the captured root and walk length. This only
// computes an address; nullopt is an addressing failure, not an EPT exit.
inline std::optional<Addr>
rootEntryAddress(const LookupInput &input, unsigned hostPhysicalBits)
{
    return entryAddress(input.config.root, input.guestPhysical,
                        input.config.walkLength, input.config.walkLength,
                        hostPhysicalBits);
}

// Locate the child entry after decoding a non-leaf at currentLevel. Denied
// permissions do not stop descent: a deeper entry may be misconfigured.
// nullopt means no child address can be formed; it does not report an exit.
inline std::optional<Addr>
nextEntryAddress(const LookupInput &input, const EntryResult &parent,
                 unsigned currentLevel, unsigned hostPhysicalBits)
{
    if (parent.kind != EntryKind::NextTable || currentLevel <= 1 ||
            currentLevel > input.config.walkLength) {
        return std::nullopt;
    }
    return entryAddress(parent.base, input.guestPhysical, currentLevel - 1,
                        input.config.walkLength, hostPhysicalBits);
}

} // namespace ept
} // namespace gem5::X86ISA

#endif // __ARCH_X86_EPT_HH__
