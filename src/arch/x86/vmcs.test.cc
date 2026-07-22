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

#include <gtest/gtest.h>

#include "arch/x86/vmcs.hh"

namespace gem5
{
namespace X86ISA
{
namespace
{

using Field = Vmcs::Field;
using FieldWidth = Vmcs::FieldWidth;
using FieldType = Vmcs::VmcsFieldType;

// Intel SDM Vol. 3C, Section 25.11.2: width and type are encoded in
// component bits 14:13 and 11:10, while bit 0 selects high access.
TEST(VmcsFieldEncoding, DecodesSdmWidthTypeAndAccessBits)
{
    EXPECT_EQ(Vmcs::widthFromEncoding(
                      Vmcs::encodingOf(Field::GuestCsSelector)),
            FieldWidth::U16);
    EXPECT_EQ(Vmcs::typeFromEncoding(
                      Vmcs::encodingOf(Field::GuestCsSelector)),
            FieldType::GuestState);
    EXPECT_EQ(Vmcs::widthFromEncoding(
                      Vmcs::encodingOf(Field::GuestIa32Efer)),
            FieldWidth::U64);
    EXPECT_EQ(Vmcs::widthFromEncoding(
                      Vmcs::encodingOf(Field::VmExitReason)),
            FieldWidth::U32);
    EXPECT_EQ(Vmcs::typeFromEncoding(
                      Vmcs::encodingOf(Field::VmExitReason)),
            FieldType::VmExitInfo);
    EXPECT_EQ(Vmcs::widthFromEncoding(
                      Vmcs::encodingOf(Field::GuestRip)),
            FieldWidth::Natural);

    Vmcs::FieldEncoding decoded;
    ASSERT_TRUE(Vmcs::decodeEncoding(
            Vmcs::encodingOf(Field::GuestIa32Efer) | 1, decoded));
    EXPECT_TRUE(decoded.highAccess());
    EXPECT_EQ(decoded.field(), Field::GuestIa32Efer);
}

// Reserved encoding bits and high access to non-64-bit fields identify an
// unsupported VMCS component; they must never alias a supported field.
TEST(VmcsFieldEncoding, RejectsReservedAndInvalidHighEncodings)
{
    Vmcs::FieldEncoding decoded;
    EXPECT_FALSE(Vmcs::decodeEncoding(1ull << 15, decoded));
    EXPECT_FALSE(Vmcs::decodeEncoding(
            Vmcs::encodingOf(Field::GuestRip) | (1u << 12), decoded));
    EXPECT_FALSE(Vmcs::fieldSupported(
            Vmcs::encodingOf(Field::GuestCr0) | 1));
    EXPECT_FALSE(Vmcs::fieldSupported(
            Vmcs::encodingOf(Field::VmExitReason) | 1));
}

// VMWRITE truncates 16- and 32-bit components to their architectural width.
TEST(VmcsFields, MasksValuesByArchitecturalWidth)
{
    Vmcs vmcs(0x2000, 1);
    uint64_t value = 0;

    ASSERT_TRUE(vmcs.write(Field::GuestCsSelector, ~0ull));
    ASSERT_TRUE(vmcs.read(Field::GuestCsSelector, value));
    EXPECT_EQ(value, 0xffff);

    ASSERT_TRUE(vmcs.write(Field::ExceptionBitmap, ~0ull));
    ASSERT_TRUE(vmcs.read(Field::ExceptionBitmap, value));
    EXPECT_EQ(value, 0xffffffff);
}

// High access changes only bits 63:32 of a 64-bit component and reads them
// zero-extended, as required by VMREAD/VMWRITE component encoding bit 0.
TEST(VmcsFields, SupportsHighAccessFor64BitFields)
{
    Vmcs vmcs(0x2000, 1);
    constexpr uint64_t initial = 0x1122334455667788ull;
    constexpr uint64_t high = 0xaabbccddu;
    uint64_t value = 0;

    ASSERT_TRUE(vmcs.write(Field::GuestIa32Efer, initial));
    ASSERT_TRUE(vmcs.write(
            Vmcs::encodingOf(Field::GuestIa32Efer) | 1, high));
    ASSERT_TRUE(vmcs.read(Field::GuestIa32Efer, value));
    EXPECT_EQ(value, 0xaabbccdd55667788ull);
    ASSERT_TRUE(vmcs.read(
            Vmcs::encodingOf(Field::GuestIa32Efer) | 1, value));
    EXPECT_EQ(value, high);
}

// VM-exit information components are read-only to VMWRITE but remain
// writable by the processor's internal VM-exit path.
TEST(VmcsFields, EnforcesReadOnlyFieldsForVmwrite)
{
    Vmcs vmcs(0x2000, 1);
    uint64_t value = 0;

    EXPECT_FALSE(Vmcs::fieldWritable(Field::VmExitReason));
    EXPECT_FALSE(vmcs.write(Field::VmExitReason, 42));
    vmcs.writeUnchecked(Field::VmExitReason, 42);
    ASSERT_TRUE(vmcs.read(Field::VmExitReason, value));
    EXPECT_EQ(value, 42);
}

// A supported but never-written component reads as zero. This protects the
// typed model from accidentally treating absent map entries as unsupported.
TEST(VmcsFields, ReadsSupportedUnsetFieldsAsZero)
{
    Vmcs vmcs(0x2000, 1);
    uint64_t value = ~0ull;

    ASSERT_TRUE(Vmcs::fieldSupported(Field::ExceptionBitmap));
    ASSERT_TRUE(Vmcs::fieldSupported(Field::Cr0GuestHostMask));
    ASSERT_TRUE(Vmcs::fieldSupported(Field::VmcsLinkPointer));
    ASSERT_TRUE(vmcs.read(Field::ExceptionBitmap, value));
    EXPECT_EQ(value, 0);
}

// Intel SDM Vol. 3C, Section 27.1: VMCLEAR makes a VMCS inactive and clear;
// a successful VMPTRLD makes it active and VMLAUNCH makes it launched.
TEST(VmcsLifecycle, ClearAndLaunchedTransitions)
{
    Vmcs vmcs(0x4000, 7);
    EXPECT_EQ(vmcs.pointer(), 0x4000);
    EXPECT_EQ(vmcs.revisionId(), 7);
    EXPECT_FALSE(vmcs.active());
    EXPECT_FALSE(vmcs.launched());

    vmcs.setActive(true);
    vmcs.setLaunched(true);
    EXPECT_TRUE(vmcs.active());
    EXPECT_TRUE(vmcs.launched());
    ASSERT_TRUE(vmcs.write(Field::GuestRip, 0x1234));

    vmcs.clear();
    EXPECT_FALSE(vmcs.active());
    EXPECT_FALSE(vmcs.launched());
    uint64_t value = ~0ull;
    ASSERT_TRUE(vmcs.read(Field::GuestRip, value));
    // VMCLEAR changes launch/active state but preserves VMCS components.
    EXPECT_EQ(value, 0x1234);
    EXPECT_EQ(vmcs.abortIndicator(), 0);
}

// Components belonging only to unadvertised advanced features must fail
// lookup instead of being accepted and silently ignored.
TEST(VmcsFields, RejectsUnadvertisedAdvancedComponents)
{
    EXPECT_FALSE(Vmcs::fieldSupported(Field::EptPointer));
    EXPECT_FALSE(Vmcs::fieldSupported(Field::VirtualApicAddress));
    EXPECT_FALSE(Vmcs::fieldSupported(Field::VmreadBitmapAddress));
    EXPECT_FALSE(Vmcs::fieldSupported(
                Field::PostedInterruptDescriptorAddress));
}

// IA32_VMX_VMCS_ENUM reports the highest supported component index in
// bits 9:1. Guest IA32_SYSENTER_CS is the highest foundational component
// in gem5-vmx's allowlist.
TEST(VmcsFields, VmcsEnumerationMatchesHighestSupportedIndex)
{
    constexpr auto encoding =
        Vmcs::encodingOf(Field::GuestSysenterCs);
    EXPECT_EQ(bits(encoding, 9, 1), 0x15);
    EXPECT_EQ(0x15u << 1, 0x2au);
}

} // namespace
} // namespace X86ISA
} // namespace gem5
