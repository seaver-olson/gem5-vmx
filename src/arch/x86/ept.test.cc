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
constexpr uint64_t AllCaps = FourLevel | FiveLevel | Uncacheable |
    WriteBack | AccessedDirty;
constexpr uint64_t Root = 0x12345000;
constexpr uint64_t FourLevelWb = Root | (3ull << 3) | 6;

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

} // namespace
} // namespace gem5::X86ISA
