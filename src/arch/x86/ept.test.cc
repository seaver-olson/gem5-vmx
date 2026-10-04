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

TEST(EptLookupInput, PreservesGuestPhysicalAddressAndAccess)
{
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    // Keep the byte offset; this is a GPA, not an aligned table address.
    constexpr Addr guestPhysical = 0x123456789abc;
    for (const auto access : {EptAccess::Read, EptAccess::Write,
                              EptAccess::Execute}) {
        const ept::LookupInput input(guestPhysical, access, *config);
        EXPECT_EQ(input.guestPhysical, guestPhysical);
        EXPECT_EQ(input.access, access);
    }
}

TEST(EptLookupInput, OwnsCapturedConfiguration)
{
    const uint64_t capturedEptp = Root | (4ull << 3) | (1ull << 6);
    auto config = ept::decodeEptp(capturedEptp, AllCaps, 48);
    const auto replacement = ept::decodeEptp(
        0x2000 | (3ull << 3) | 6, AllCaps, 48);
    ASSERT_TRUE(config);
    ASSERT_TRUE(replacement);
    const ept::LookupInput input(0x1234, EptAccess::Read, *config);

    // Change every source field, then destroy the source configuration.
    // The lookup must retain the snapshot supplied at construction.
    *config = *replacement;
    config.reset();
    EXPECT_EQ(input.config.eptp, capturedEptp);
    EXPECT_EQ(input.config.root, Root);
    EXPECT_EQ(input.config.walkLength, 5);
    EXPECT_EQ(input.config.memoryType, 0);
    EXPECT_TRUE(input.config.accessedDirty);
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

TEST(EptRootEntryAddress, SelectsCapturedRootAndStartingLevel)
{
    const auto fourLevel = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    constexpr Addr fiveLevelRoot = 0x56789000;
    const auto fiveLevel = ept::decodeEptp(
        fiveLevelRoot | (4ull << 3) | 6, AllCaps, 48);
    ASSERT_TRUE(fourLevel);
    ASSERT_TRUE(fiveLevel);
    const Addr fourLevelGpa = (0x12ull << 39) | (0x34ull << 30) | 0xabc;
    const ept::LookupInput fourInput(
        fourLevelGpa, EptAccess::Read, *fourLevel);
    const ept::LookupInput fiveInput(
        (0x1aull << 48) | fourLevelGpa, EptAccess::Execute, *fiveLevel);

    EXPECT_EQ(ept::rootEntryAddress(fourInput, 48), Root + 0x12 * 8);
    EXPECT_EQ(ept::rootEntryAddress(fiveInput, 48), fiveLevelRoot + 0x1a * 8);
}

TEST(EptRootEntryAddress, KeepsGuestAndHostAddressBoundsSeparate)
{
    for (unsigned levels : {4u, 5u}) {
        const Addr maxGpa = (1ull << (12 + 9 * levels)) - 1;
        for (unsigned width : {32u, 48u, 52u}) {
            const Addr root = (1ull << width) - 4096;
            const auto config = ept::decodeEptp(
                root | (uint64_t(levels - 1) << 3) | 6, AllCaps, width);
            ASSERT_TRUE(config);
            const ept::LookupInput last(maxGpa, EptAccess::Write, *config);
            const ept::LookupInput overflow(
                maxGpa + 1, EptAccess::Write, *config);

            EXPECT_EQ(ept::rootEntryAddress(last, width),
                      (1ull << width) - 8);
            EXPECT_FALSE(ept::rootEntryAddress(overflow, width));
        }
    }
}

TEST(EptRootEntryAddress, RejectsInvalidHostWidthAndOutOfRangeRoot)
{
    const auto config = ept::decodeEptp(
        (1ull << 40) | (3ull << 3) | 6, AllCaps, 48);
    ASSERT_TRUE(config);
    const ept::LookupInput input(0x1234, EptAccess::Read, *config);
    EXPECT_FALSE(ept::rootEntryAddress(input, 31));
    EXPECT_FALSE(ept::rootEntryAddress(input, 53));
    EXPECT_FALSE(ept::rootEntryAddress(input, 32));
}

TEST(EptNextEntryAddress, SelectsEachChildInFourAndFiveLevelWalks)
{
    const Addr fourLevelGpa = (0x12ull << 39) | (0x34ull << 30) |
        (0x56ull << 21) | (0x78ull << 12) | 0x9ab;
    const Addr indices[] = {0, 0x78, 0x56, 0x34, 0x12};
    for (unsigned levels : {4u, 5u}) {
        const auto config = ept::decodeEptp(
            Root | (uint64_t(levels - 1) << 3) | 6, AllCaps, 48);
        ASSERT_TRUE(config);
        const Addr gpa = fourLevelGpa | (levels == 5 ? 0x1aull << 48 : 0);
        const ept::LookupInput input(gpa, EptAccess::Read, *config);
        for (unsigned level = levels; level > 1; --level) {
            const Addr nextTable = 0x1000 * level;
            const auto parent = ept::decodeEntry(
                nextTable | 7, level, 48, AllCaps, 7, input.access);
            ASSERT_TRUE(parent);
            ASSERT_EQ(parent->kind, ept::EntryKind::NextTable);
            EXPECT_EQ(ept::nextEntryAddress(input, *parent, level, 48),
                      nextTable + indices[level - 1] * 8);
        }
    }
}

TEST(EptNextEntryAddress, RejectsTerminalResultsAndInvalidLevels)
{
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    const ept::LookupInput input(0x1234, EptAccess::Read, *config);
    for (auto kind : {ept::EntryKind::Mapping, ept::EntryKind::Violation,
                      ept::EntryKind::Misconfiguration}) {
        const ept::EntryResult terminal{kind, 0x2000};
        EXPECT_FALSE(ept::nextEntryAddress(input, terminal, 3, 48));
    }
    const ept::EntryResult parent{ept::EntryKind::NextTable, 0x2000};
    for (unsigned level : {0u, 1u, 5u, 6u, ~0u}) {
        EXPECT_FALSE(ept::nextEntryAddress(input, parent, level, 48));
    }
}

TEST(EptNextEntryAddress, ContinuesAfterInheritedPermissionDenial)
{
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    const ept::LookupInput input(0x3456, EptAccess::Write, *config);
    const auto parent = ept::decodeEntry(0x2001, 2, 48, AllCaps, 0,
                                         input.access);
    ASSERT_TRUE(parent);
    ASSERT_EQ(parent->kind, ept::EntryKind::NextTable);
    ASSERT_EQ(parent->permissions, 0);
    EXPECT_EQ(ept::nextEntryAddress(input, *parent, 2, 48), 0x2000 + 3 * 8);
}

TEST(EptNextEntryAddress, RejectsInvalidChildAddressInputs)
{
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    const ept::LookupInput input(0x1234, EptAccess::Read, *config);
    const ept::EntryResult parent{ept::EntryKind::NextTable, 0x2000};
    EXPECT_FALSE(ept::nextEntryAddress(input, parent, 3, 31));
    EXPECT_FALSE(ept::nextEntryAddress(input, parent, 3, 53));
    for (Addr base : {0x2001ull, 1ull << 48}) {
        const ept::EntryResult invalid{ept::EntryKind::NextTable, base};
        EXPECT_FALSE(ept::nextEntryAddress(input, invalid, 3, 48));
    }
    const ept::LookupInput overflow(1ull << 48, EptAccess::Read, *config);
    EXPECT_FALSE(ept::nextEntryAddress(overflow, parent, 3, 48));
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
