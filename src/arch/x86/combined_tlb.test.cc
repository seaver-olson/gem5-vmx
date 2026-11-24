/* Copyright (c) 2026 Seaver Olson, Loyola University Chicago
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>

#include "arch/x86/combined_tlb.hh"

namespace gem5::X86ISA
{
namespace
{

TEST(CombinedTLB, RangeAndContext)
{
    CombinedTLB cache(2);
    CombinedEntry entry{};
    entry.tag = {1, 0x3000, 5, 0x101e};
    entry.linearBase = 0x4000;
    entry.shift = 12;
    cache.insert(entry);
    EXPECT_TRUE(cache.lookup(entry.tag, 0x4fff, 1));
    EXPECT_FALSE(cache.lookup(entry.tag, 0x4fff, 2));
    EXPECT_FALSE(cache.lookup(entry.tag, 0x3fff, 2));
    for (const auto &tag :
         {CombinedTag{0, 0x3000, 5, 0x101e}, CombinedTag{1, 0x4000, 5, 0x101e},
          CombinedTag{1, 0x3000, 6, 0x101e},
          CombinedTag{1, 0x3000, 5, 0x105e}}) {
        EXPECT_FALSE(cache.lookup(tag, 0x4000, 1));
    }
    cache.invalidateThread(0);
    EXPECT_TRUE(cache.lookup(entry.tag, 0x4000, 1));
    cache.invalidateThread(1);
    EXPECT_FALSE(cache.lookup(entry.tag, 0x4000, 1));
}

TEST(CombinedTLB, Replacement)
{
    CombinedTLB cache(2);
    CombinedEntry entry{};
    entry.shift = 12;
    EXPECT_FALSE(cache.insert(entry));
    entry.linearBase = 0x1000;
    EXPECT_FALSE(cache.insert(entry));
    EXPECT_TRUE(cache.lookup(entry.tag, 0, 1));
    entry.linearBase = 0x2000;
    EXPECT_TRUE(cache.insert(entry));
    EXPECT_FALSE(cache.lookup(entry.tag, 0x1000, 1));
    EXPECT_TRUE(cache.lookup(entry.tag, 0, 1));
    cache.clear();
    EXPECT_FALSE(cache.lookup(entry.tag, 0, 1));
}

} // anonymous namespace
} // namespace gem5::X86ISA
