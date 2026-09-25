/* SPDX-License-Identifier: BSD-3-Clause */
#include <array>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "arch/x86/paging.hh"

namespace gem5::X86ISA::paging
{
namespace
{
TEST(PaePdpte, ReservedPresentAndIgnoredBits)
{
    for (unsigned width : {32, 36, 48, 52}) {
        for (unsigned bit = 1; bit < 64; ++bit) {
            const uint64_t value = 1ULL << bit;
            EXPECT_TRUE(validPaePdpte(value, width)); // nonpresent
            const bool allowed = bit == 3 || bit == 4 ||
                (bit >= 9 && bit < width);
            EXPECT_EQ(validPaePdpte(value | 1, width), allowed)
                << "width=" << width << " bit=" << bit;
        }
    }
}

TEST(PaePdpte, ControlRegisterReloadTriggers)
{
    const uint64_t pg = 1ULL << 31, pae = 1ULL << 5;
    EXPECT_TRUE(reloadPaePdpte(0, 0, pae, 0, pg));
    EXPECT_TRUE(reloadPaePdpte(4, pg, 0, 0, pae));
    EXPECT_TRUE(reloadPaePdpte(3, pg, pae, 0, 0x12345000));
    for (unsigned bit : {29, 30})
        EXPECT_TRUE(reloadPaePdpte(0, pg, pae, 0, pg | (1ULL << bit)));
    for (unsigned bit : {4, 7, 20})
        EXPECT_TRUE(reloadPaePdpte(4, pg, pae, 0, pae | (1ULL << bit)));
    EXPECT_FALSE(reloadPaePdpte(0, pg, pae, 0, pg | (1ULL << 16)));
    EXPECT_FALSE(reloadPaePdpte(4, pg, pae, 0, pae));
    EXPECT_FALSE(reloadPaePdpte(0, pg, pae, 0, 0));
    EXPECT_FALSE(reloadPaePdpte(3, 0, pae, 0, 0));
    EXPECT_FALSE(reloadPaePdpte(3, pg, 0, 0, 0));
    EXPECT_FALSE(reloadPaePdpte(3, pg, pae, 1ULL << 8, 0));
}

template <class T>
T update(T initial, T observed, T flags)
{
    // Exercise an unaligned descriptor and guard bytes on both sides.
    std::array<uint8_t, sizeof(T) + 2> storage;
    storage.fill(0x5a);
    T raw = htole(initial);
    std::memcpy(storage.data() + 1, &raw, sizeof(T));
    ConditionalUpdate<T> operation(observed, flags);
    std::unique_ptr<AtomicOpFunctor> copy(operation.clone());
    (*copy)(storage.data() + 1);
    std::memcpy(&raw, storage.data() + 1, sizeof(T));
    EXPECT_EQ(storage.front(), 0x5a);
    EXPECT_EQ(storage.back(), 0x5a);
    return letoh(raw);
}

TEST(PagingUpdate, SetsOnlyRequestedBits)
{
    const uint64_t descriptor = 0x8000123456789007;
    EXPECT_EQ(update<uint64_t>(descriptor, descriptor, 0x60), descriptor | 0x60);
    EXPECT_EQ(update<uint32_t>(0x12345007, 0x12345007, 0x60), 0x12345067);
}

TEST(PagingUpdate, ConcurrentAccessedThenDirty)
{
    const uint64_t descriptor = 0x12345007;
    const auto accessed = update<uint64_t>(descriptor, descriptor, 0x20);
    // A concurrent update invalidates the old comparison. A rewalk observes
    // the new value, preserving A when setting D.
    EXPECT_EQ(update<uint64_t>(accessed, descriptor, 0x40), accessed);
    EXPECT_EQ(update<uint64_t>(accessed, accessed, 0x40), descriptor | 0x60);
}

TEST(PagingUpdate, DoesNotOverwriteReplacement)
{
    for (uint64_t changed : {1ull << 12, 1ull << 1, 1ull << 2,
                             1ull << 63, 1ull << 9, 1ull << 0}) {
        const uint64_t observed = 0x12345007;
        const uint64_t replacement = observed ^ changed;
        EXPECT_EQ(update<uint64_t>(replacement, observed, 0x60), replacement);
    }
    EXPECT_EQ(update<uint32_t>(0xabcdef07, 0x12345007, 0x60), 0xabcdef07);
}

TEST(PagingAddress, InstructionFetchFaultAddress)
{
    EXPECT_EQ(faultAddress(0x8000, 64, 0x8013), 0x8013);
    EXPECT_EQ(faultAddress(0x9000, 64, 0x8fff), 0x9000);
    EXPECT_EQ(faultAddress(0x8000, 64, 0x9000), 0x8000);
    EXPECT_EQ(faultAddress(0xfffffffffffff000, 64, 0xfffffffffffff003),
              0xfffffffffffff003);
}

TEST(PagingAddress, PhysicalWidthAndIgnoredHighBits)
{
    for (unsigned width : {36u, 48u, 52u}) {
        EXPECT_FALSE(reservedAddress(mask(width) & ~mask(12), width));
        EXPECT_FALSE(reservedAddress(0xfff0000000000000, width));
        for (unsigned bit = width; bit < 52; bit++)
            EXPECT_TRUE(reservedAddress(1ull << bit, width));
    }
}
} // anonymous namespace
} // namespace gem5::X86ISA::paging
