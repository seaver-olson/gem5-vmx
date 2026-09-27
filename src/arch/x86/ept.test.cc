/* SPDX-License-Identifier: BSD-3-Clause */

#include <gtest/gtest.h>

#include "arch/x86/ept.hh"
#include "arch/x86/translation.hh"

namespace gem5::X86ISA
{
namespace
{

constexpr uint64_t FourLevel = 1ull << 6;
constexpr uint64_t FiveLevel = 1ull << 7;
constexpr uint64_t Uncacheable = 1ull << 8;
constexpr uint64_t WriteBack = 1ull << 14;
constexpr uint64_t AccessedDirty = 1ull << 21;
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

TEST(EptWalkPlan, SelectsFourAndFiveLevelEntries)
{
    const Addr fourLevelGpa = (0x12ull << 39) | (0x34ull << 30) |
        (0x56ull << 21) | (0x78ull << 12) | 0x9ab;
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    const EptWalkRequest request{*config, fourLevelGpa,
        EptAccess::Read, EptOrigin::GuestPageTable, 48};
    const auto plan = ept::WalkPlan::create(request);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->input().origin, EptOrigin::GuestPageTable);
    EXPECT_EQ(plan->rootEntryAddress(), Root + 0x12 * 8);
    EXPECT_EQ(plan->entryAddress(0x2000, 3), 0x2000 + 0x34 * 8);
    EXPECT_EQ(plan->entryAddress(0x3000, 2), 0x3000 + 0x56 * 8);
    EXPECT_EQ(plan->entryAddress(0x4000, 1), 0x4000 + 0x78 * 8);

    const auto fiveLevel = ept::decodeEptp(
        Root | (4ull << 3) | 6, AllCaps, 48);
    ASSERT_TRUE(fiveLevel);
    const auto fivePlan = ept::WalkPlan::create(EptWalkRequest{
        *fiveLevel, (0x1aull << 48) | fourLevelGpa,
        EptAccess::Execute, EptOrigin::FinalAccess, 48});
    ASSERT_TRUE(fivePlan);
    EXPECT_EQ(fivePlan->rootEntryAddress(), Root + 0x1a * 8);
    EXPECT_EQ(fivePlan->entryAddress(0x2000, 4), 0x2000 + 0x12 * 8);
}

TEST(EptWalkPlan, RejectsOutOfRangeAddressesAndLevels)
{
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    const EptWalkRequest request{*config, 0x1234,
        EptAccess::Write, EptOrigin::FinalAccess, 48};
    auto plan = ept::WalkPlan::create(request);
    ASSERT_TRUE(plan);
    EXPECT_FALSE(plan->entryAddress(0x2001, 3));
    EXPECT_FALSE(plan->entryAddress(1ull << 48, 3));
    EXPECT_FALSE(plan->entryAddress(0x2000, 0));
    EXPECT_FALSE(plan->entryAddress(0x2000, 5));

    auto invalid = request;
    invalid.guestPhysical = 1ull << 48;
    EXPECT_FALSE(ept::WalkPlan::create(invalid));
    invalid = request;
    invalid.config.root |= 1;
    EXPECT_FALSE(ept::WalkPlan::create(invalid));
    invalid = request;
    invalid.hostPhysicalBits = 31;
    EXPECT_FALSE(ept::WalkPlan::create(invalid));
}

TEST(EptWalkPlan, ContextCarriesCapturedConfigurationIntoRequest)
{
    TranslationContext context{};
    EXPECT_FALSE(context.eptWalkRequest(0x2000, EptAccess::Read,
                                       EptOrigin::FinalAccess));
    const auto config = ept::decodeEptp(FourLevelWb, AllCaps, 48);
    ASSERT_TRUE(config);
    context.ept = *config;
    context.physicalBits = 48;
    const auto request = context.eptWalkRequest(
        0x2345, EptAccess::Write, EptOrigin::GuestPageTable);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->config.eptp, FourLevelWb);
    EXPECT_EQ(request->guestPhysical, 0x2345);
    EXPECT_EQ(request->access, EptAccess::Write);
    EXPECT_EQ(request->origin, EptOrigin::GuestPageTable);
    EXPECT_EQ(request->hostPhysicalBits, 48);
}

} // namespace
} // namespace gem5::X86ISA
