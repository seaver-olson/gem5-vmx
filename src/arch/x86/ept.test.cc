/* SPDX-License-Identifier: BSD-3-Clause */

#include <gtest/gtest.h>

#include "arch/x86/ept.hh"

namespace gem5::X86ISA
{
namespace
{

constexpr uint64_t FourLevel = 1ull << 6;
constexpr uint64_t FiveLevel = 1ull << 7;
constexpr uint64_t Uncacheable = 1ull << 8;
constexpr uint64_t WriteBack = 1ull << 14;
constexpr uint64_t AccessedDirty = 1ull << 21;
constexpr uint64_t ExecuteOnly = 1ull;
constexpr uint64_t TwoMib = 1ull << 16;
constexpr uint64_t OneGib = 1ull << 17;
constexpr uint64_t AllCaps = FourLevel | FiveLevel | Uncacheable |
    WriteBack | AccessedDirty;
constexpr uint64_t Root = 0x12345000;
constexpr uint64_t FourLevelWb = Root | (3ull << 3) | 6;

TEST(Eptp, FieldAvailabilityFollowsExecutionControlCapabilities)
{
    const uint64_t primary = uint64_t(ept::ActivateSecondaryControls) << 32;
    const uint64_t secondary =
        uint64_t(ept::EnableEptSecondaryControl) << 32;
    EXPECT_FALSE(ept::secondaryControlsAvailable(0));
    EXPECT_FALSE(ept::eptPointerAvailable(0, secondary));
    EXPECT_FALSE(ept::eptPointerAvailable(primary, 0));
    EXPECT_TRUE(ept::secondaryControlsAvailable(primary));
    EXPECT_TRUE(ept::eptPointerAvailable(primary, secondary));
}

TEST(Eptp, DecodesSupportedConfiguration)
{
    auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->eptp, FourLevelWb);
    EXPECT_EQ(config->root, Root);
    EXPECT_EQ(config->walkLength, 4);
    EXPECT_EQ(config->memoryType, 6);
    EXPECT_FALSE(config->accessedDirty);

    config = ept::decodeEptp(Root | (4ull << 3) | (1ull << 6),
                            AllCaps, 48);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->walkLength, 5);
    EXPECT_EQ(config->memoryType, 0);
    EXPECT_TRUE(config->accessedDirty);
}

TEST(Eptp, RejectsUnsupportedCapabilities)
{
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb, FourLevel, 48));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb, WriteBack, 48));
    EXPECT_FALSE(ept::decodeEptp(Root | (4ull << 3),
                                 FourLevel | Uncacheable, 48));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb | (1ull << 6),
                                 FourLevel | WriteBack, 48));
}

TEST(Eptp, RejectsReservedBitsAndPhysicalAddressOverflow)
{
    EXPECT_FALSE(ept::decodeEptp(Root | (3ull << 3) | 1, AllCaps, 48));
    EXPECT_FALSE(ept::decodeEptp(Root | (2ull << 3) | 6, AllCaps, 48));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb | (1ull << 7), AllCaps, 48));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb | (1ull << 52), AllCaps, 52));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb | (1ull << 48), AllCaps, 48));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb, AllCaps, 31));
    EXPECT_FALSE(ept::decodeEptp(FourLevelWb, AllCaps, 53));
}

TEST(EptEntryAddress, SelectsFourAndFiveLevelEntries)
{
    const Addr fourLevelGpa = (0x12ull << 39) | (0x34ull << 30) |
        (0x56ull << 21) | (0x78ull << 12) | 0x9ab;
    EXPECT_EQ(ept::entryAddress(Root, fourLevelGpa, 4, 4, 48),
              Root + 0x12 * 8);
    EXPECT_EQ(ept::entryAddress(0x2000, fourLevelGpa, 3, 4, 48),
              0x2000 + 0x34 * 8);
    EXPECT_EQ(ept::entryAddress(0x3000, fourLevelGpa, 2, 4, 48),
              0x3000 + 0x56 * 8);
    EXPECT_EQ(ept::entryAddress(0x4000, fourLevelGpa, 1, 4, 48),
              0x4000 + 0x78 * 8);

    const Addr fiveLevelGpa = (0x1aull << 48) | fourLevelGpa;
    EXPECT_EQ(ept::entryAddress(Root, fiveLevelGpa, 5, 5, 48),
              Root + 0x1a * 8);
    EXPECT_EQ(ept::entryAddress(0x2000, fiveLevelGpa, 4, 5, 48),
              0x2000 + 0x12 * 8);
}

TEST(EptEntryAddress, RejectsOutOfRangeAddressesAndLevels)
{
    EXPECT_FALSE(ept::entryAddress(0x2001, 0x1234, 3, 4, 48));
    EXPECT_FALSE(ept::entryAddress(1ull << 48, 0x1234, 3, 4, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 0, 4, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 5, 4, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 6, 5, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 1, 3, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 1, 6, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 1ull << 48, 4, 4, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 1ull << 57, 5, 5, 48));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 1, 4, 31));
    EXPECT_FALSE(ept::entryAddress(0x2000, 0x1234, 1, 4, 53));
}

TEST(EptEntryAddress, LastTableSlotStaysWithinHostAddressWidth)
{
    for (unsigned width : {32u, 48u, 52u}) {
        const Addr tableBase = (1ull << width) - 4096;
        EXPECT_EQ(ept::entryAddress(tableBase, (1ull << 48) - 1,
                                   1, 4, width),
                  (1ull << width) - sizeof(uint64_t));
    }
}

TEST(EptEntry, MapsFourKibPageAndCarriesPermissions)
{
    const auto table = ept::decodeEntry(0x2000 | 5, 4, 48, AllCaps,
                                         7, EptAccess::Read);
    ASSERT_TRUE(table);
    EXPECT_EQ(table->kind, ept::EntryKind::NextTable);
    EXPECT_EQ(table->base, 0x2000);
    EXPECT_EQ(table->permissions, 5);

    const auto page = ept::decodeEntry(0x12345000 | (6ull << 3) | 7,
                                        1, 48, AllCaps, table->permissions,
                                        EptAccess::Execute);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->kind, ept::EntryKind::Mapping);
    EXPECT_EQ(page->base, 0x12345000);
    EXPECT_EQ(page->pageShift, 12);
    EXPECT_EQ(page->memoryType, 6);
    EXPECT_EQ(page->permissions, 5);
    EXPECT_EQ(ept::decodeEntry(0x12345000 | (6ull << 3) | 7,
                               1, 48, AllCaps, table->permissions,
                               EptAccess::Write)->kind,
              ept::EntryKind::Violation);
}

TEST(EptEntry, DenialWaitsUntilLeafSoMisconfigurationWins)
{
    const auto parent = ept::decodeEntry(0x2000 | 1, 2, 48, AllCaps,
                                          7, EptAccess::Write);
    ASSERT_TRUE(parent);
    EXPECT_EQ(parent->kind, ept::EntryKind::NextTable);
    EXPECT_EQ(parent->permissions, 1);
    EXPECT_EQ(ept::decodeEntry(0x3000 | 7 | (2ull << 3), 1, 48,
                               AllCaps, parent->permissions,
                               EptAccess::Write)->kind,
              ept::EntryKind::Misconfiguration);
    EXPECT_EQ(ept::decodeEntry(0x3000 | 7 | (6ull << 3), 1, 48,
                               AllCaps, parent->permissions,
                               EptAccess::Write)->kind,
              ept::EntryKind::Violation);
}

TEST(EptEntry, AbsentEntriesCauseViolationBeforeReservedBitChecks)
{
    for (const uint64_t raw : {0ull, 1ull << 48, ~7ull}) {
        EXPECT_EQ(ept::decodeEntry(raw, 3, 48, AllCaps, 7,
                                   EptAccess::Read)->kind,
                  ept::EntryKind::Violation);
    }
}

TEST(EptEntry, RejectsMalformedPermissionsAddressesAndMemoryTypes)
{
    const auto kind = [](uint64_t raw, unsigned level, uint64_t caps = AllCaps) {
        return ept::decodeEntry(raw, level, 48, caps, 7,
                                EptAccess::Read)->kind;
    };
    EXPECT_EQ(kind(0x1000 | 2, 1), ept::EntryKind::Misconfiguration);
    EXPECT_EQ(kind(0x1000 | 4, 1), ept::EntryKind::Misconfiguration);
    EXPECT_EQ(ept::decodeEntry(0x1000 | 4, 1, 48,
                               AllCaps | ExecuteOnly, 7,
                               EptAccess::Execute)->kind,
              ept::EntryKind::Mapping);
    EXPECT_EQ(kind(0x1000 | 4, 1, AllCaps | ExecuteOnly),
              ept::EntryKind::Violation);
    EXPECT_EQ(kind((1ull << 48) | 7, 1), ept::EntryKind::Misconfiguration);
    EXPECT_EQ(kind(0x1000 | 7 | (1ull << 3), 4),
              ept::EntryKind::Misconfiguration);
    EXPECT_EQ(kind(0x1000 | 7 | (1ull << 7), 4),
              ept::EntryKind::Misconfiguration);
    for (unsigned memoryType : {2u, 3u, 7u}) {
        EXPECT_EQ(kind(0x1000 | 7 | (uint64_t(memoryType) << 3), 1),
                  ept::EntryKind::Misconfiguration);
    }
    for (unsigned memoryType : {0u, 1u, 4u, 5u, 6u}) {
        EXPECT_EQ(kind(0x1000 | 7 | (uint64_t(memoryType) << 3), 1),
                  ept::EntryKind::Mapping);
    }
    // PTE bit 7 and feature-disabled high software bits are ignored.
    EXPECT_EQ(kind(0x1000 | 7 | (1ull << 7) | (1ull << 63), 1),
              ept::EntryKind::Mapping);
}

TEST(EptEntry, LargePagesRequireCapabilityAndAlignment)
{
    const auto decode = [](uint64_t raw, unsigned level, uint64_t caps) {
        return ept::decodeEntry(raw, level, 48, caps, 7, EptAccess::Read);
    };
    const uint64_t twoMib = 0x200000 | 7 | (6ull << 3) | (1ull << 7);
    const auto pde = decode(twoMib, 2, AllCaps | TwoMib);
    ASSERT_TRUE(pde);
    EXPECT_EQ(pde->kind, ept::EntryKind::Mapping);
    EXPECT_EQ(pde->base, 0x200000);
    EXPECT_EQ(pde->pageShift, 21);
    EXPECT_EQ(decode(twoMib, 2, AllCaps)->kind,
              ept::EntryKind::Misconfiguration);
    EXPECT_EQ(decode(twoMib | (1ull << 12), 2, AllCaps | TwoMib)->kind,
              ept::EntryKind::Misconfiguration);

    const uint64_t oneGib = 0x80000000 | 7 | (6ull << 3) | (1ull << 7);
    const auto pdpte = decode(oneGib, 3, AllCaps | OneGib);
    ASSERT_TRUE(pdpte);
    EXPECT_EQ(pdpte->kind, ept::EntryKind::Mapping);
    EXPECT_EQ(pdpte->base, 0x80000000);
    EXPECT_EQ(pdpte->pageShift, 30);
    EXPECT_EQ(decode(oneGib, 3, AllCaps)->kind,
              ept::EntryKind::Misconfiguration);
    EXPECT_EQ(decode(oneGib | (1ull << 21), 3, AllCaps | OneGib)->kind,
              ept::EntryKind::Misconfiguration);
}

TEST(EptEntry, RejectsInvalidDecoderArguments)
{
    EXPECT_FALSE(ept::decodeEntry(0x1007, 0, 48, AllCaps, 7,
                                  EptAccess::Read));
    EXPECT_FALSE(ept::decodeEntry(0x1007, 6, 48, AllCaps, 7,
                                  EptAccess::Read));
    EXPECT_FALSE(ept::decodeEntry(0x1007, 1, 53, AllCaps, 7,
                                  EptAccess::Read));
    EXPECT_FALSE(ept::decodeEntry(0x1007, 1, 48, AllCaps, 8,
                                  EptAccess::Read));
    EXPECT_FALSE(ept::decodeEntry(0x1007, 1, 48, AllCaps, 7,
                                  static_cast<EptAccess>(3)));
}

} // namespace
} // namespace gem5::X86ISA
