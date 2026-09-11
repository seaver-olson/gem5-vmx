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

#include "arch/x86/vmx_utils.hh"

namespace gem5
{
namespace X86ISA
{
namespace
{

// Appendix A capability MSRs encode required-one bits in the low dword and
// allowed-one bits in the high dword.
TEST(VmxControls, CapabilityMaskValidation)
{
    constexpr uint32_t required = 1u << 5;
    constexpr uint32_t allowed = required | (1u << 9);
    constexpr uint64_t capability =
        vmx::controlCapabilityMsr(required, allowed);

    EXPECT_FALSE(vmx::controlsAllowed(0, capability));
    EXPECT_TRUE(vmx::controlsAllowed(required, capability));
    EXPECT_TRUE(vmx::controlsAllowed(allowed, capability));
    EXPECT_FALSE(vmx::controlsAllowed(allowed | (1u << 17), capability));
}

// IA32_VMX_CR{0,4}_FIXED0 requires ones and FIXED1 permits ones.
TEST(VmxControls, FixedBitValidation)
{
    EXPECT_TRUE(vmx::fixedBitsAllowed(0b0011, 0b0001, 0b0111));
    EXPECT_FALSE(vmx::fixedBitsAllowed(0b0010, 0b0001, 0b0111));
    EXPECT_FALSE(vmx::fixedBitsAllowed(0b1001, 0b0001, 0b0111));
    EXPECT_TRUE(vmx::fixedBitsAllowed(0b1001, 0b0001, 0b0111,
            0b1000));
}

// With PCID unavailable, IA-32e and legacy non-PAE CR3 permit only PWT/PCD
// below bit 12. Addresses at or above the advertised physical width fail.
TEST(VmxCr3, Ia32eAndLegacyReservedBits)
{
    uint64_t cr4 = 0;
    EXPECT_TRUE(vmx::validCr3(0x12345000, cr4, false, 48));
    EXPECT_TRUE(vmx::validCr3(0x12345018, cr4, false, 48));
    EXPECT_FALSE(vmx::validCr3(0x12345001, cr4, false, 48));
    EXPECT_FALSE(vmx::validCr3(1ull << 48, cr4, false, 48));

    cr4 |= 1ull << 5;
    EXPECT_TRUE(vmx::validCr3(0x12345018, cr4, true, 48));
    EXPECT_FALSE(vmx::validCr3(0x12345001, cr4, true, 48));
}

// Legacy PAE loads four PDPTEs from a 32-byte-aligned CR3 base.
TEST(VmxCr3, LegacyPaeRequiresThirtyTwoByteAlignment)
{
    uint64_t cr4 = 1ull << 5;
    EXPECT_TRUE(vmx::validCr3(0x12345020, cr4, false, 48));
    EXPECT_FALSE(vmx::validCr3(0x12345018, cr4, false, 48));
}

// CR4.PCIDE is not exposed by CPUID or VMX fixed masks in this model, so a
// PCID-form VMCS CR3 is rejected rather than interpreted approximately.
TEST(VmxCr3, RejectsPcideMode)
{
    uint64_t cr4 = (1ull << 5) | (1ull << 17);
    EXPECT_FALSE(vmx::validCr3(0x12345001, cr4, true, 48));
}

// VM-entry/exit CR0 loading preserves ET, CD, NW, and reserved bits while
// replacing the architecturally loadable bits from the VMCS.
TEST(VmxControlState, Cr0LoadPreservesUnloadedBits)
{
    constexpr uint64_t preserved =
        (1ull << 4) | (1ull << 29) | (1ull << 30) | (1ull << 40);
    constexpr uint64_t vmcsValue =
        (1ull << 0) | (1ull << 5) | (1ull << 16) | (1ull << 31);
    const uint64_t merged = vmx::mergeLoadedCr0(preserved, vmcsValue);

    EXPECT_EQ(merged & preserved, preserved);
    EXPECT_EQ(merged & vmx::LoadedCr0Bits, vmcsValue);
}

// SDM 28.1.3 gives CLTS and LMSW distinct exit tests. In particular LMSW
// cannot clear PE, so source PE=0 must not exit solely because live PE=1.
TEST(VmxControlState, CltsAndLmswExitConditions)
{
    EXPECT_TRUE(vmx::cltsCausesExit(1u << 3, 1u << 3));
    EXPECT_FALSE(vmx::cltsCausesExit(1u << 3, 0));

    EXPECT_FALSE(vmx::lmswCausesExit(1, 0, 0));
    EXPECT_TRUE(vmx::lmswCausesExit(1, 0, 1));
    EXPECT_FALSE(vmx::lmswCausesExit(1, 1, 0));
    EXPECT_TRUE(vmx::lmswCausesExit(1u << 2, 0, 1u << 2));
}

// The SDM's unconditional VM exits occur only after higher-priority
// instruction-recognition and privilege faults have been resolved.
TEST(VmxInstructionPriority, PreExitFaultsWin)
{
    using Fault = vmx::InstructionFault;

    EXPECT_EQ(vmx::invdPreExitFault(0), Fault::None);
    EXPECT_EQ(vmx::invdPreExitFault(1), Fault::GeneralProtection);
    EXPECT_EQ(vmx::invdPreExitFault(2), Fault::GeneralProtection);
    EXPECT_EQ(vmx::invdPreExitFault(3), Fault::GeneralProtection);

    EXPECT_EQ(vmx::xsetbvPreExitFault(0, true, true), Fault::None);
    EXPECT_EQ(vmx::xsetbvPreExitFault(1, true, true),
              Fault::GeneralProtection);
    EXPECT_EQ(vmx::xsetbvPreExitFault(2, true, true),
              Fault::GeneralProtection);
    EXPECT_EQ(vmx::xsetbvPreExitFault(3, true, true),
              Fault::GeneralProtection);
    EXPECT_EQ(vmx::xsetbvPreExitFault(0, false, true),
              Fault::InvalidOpcode);
    EXPECT_EQ(vmx::xsetbvPreExitFault(3, false, true),
              Fault::InvalidOpcode);
    EXPECT_EQ(vmx::xsetbvPreExitFault(0, true, false),
              Fault::InvalidOpcode);
    EXPECT_EQ(vmx::xsetbvPreExitFault(3, true, false),
              Fault::InvalidOpcode);

    EXPECT_EQ(vmx::getsecPreExitFault(0, true), Fault::None);
    EXPECT_EQ(vmx::getsecPreExitFault(1, true), Fault::GeneralProtection);
    EXPECT_EQ(vmx::getsecPreExitFault(2, true), Fault::GeneralProtection);
    EXPECT_EQ(vmx::getsecPreExitFault(3, true), Fault::GeneralProtection);
    EXPECT_EQ(vmx::getsecPreExitFault(0, false), Fault::InvalidOpcode);
    EXPECT_EQ(vmx::getsecPreExitFault(3, false), Fault::InvalidOpcode);
}

// Only #DF/#TS/#NP/#SS/#GP/#PF/#AC push an error code on real hardware; every
// other vector, including ones adjacent to this set, must not.
TEST(VmxEventInjection, HardwareExceptionRequiresErrorCode)
{
    for (uint8_t vector : {8, 10, 11, 12, 13, 14, 17}) {
        EXPECT_TRUE(vmx::hardwareExceptionRequiresErrorCode(vector))
            << "vector " << static_cast<int>(vector);
    }
    for (uint8_t vector : {0, 1, 2, 3, 4, 5, 6, 7, 9, 15, 16, 18, 31}) {
        EXPECT_FALSE(vmx::hardwareExceptionRequiresErrorCode(vector))
            << "vector " << static_cast<int>(vector);
    }
}

// SDM Vol. 3C 26.2.1.3: deliver-error-code must track the vector/type
// exactly, in both directions, and only for the hardware-exception class.
TEST(VmxEventInjection, ErrorCodeBitMustMatchVectorAndType)
{
    // #GP (13) is a hardware exception that requires an error code.
    EXPECT_TRUE(vmx::eventInjectionErrorCodeValid(
            /*isHardwareException=*/true, /*vector=*/13,
            /*deliverErrorCode=*/true));
    EXPECT_FALSE(vmx::eventInjectionErrorCodeValid(
            true, 13, /*deliverErrorCode=*/false));

    // #DE (0) is a hardware exception that never carries an error code.
    EXPECT_TRUE(vmx::eventInjectionErrorCodeValid(true, 0, false));
    EXPECT_FALSE(vmx::eventInjectionErrorCodeValid(true, 0, true));

    // The deliver-error-code bit is only meaningful for the hardware-
    // exception class; any other class must never set it, even for a
    // vector that would require one as a hardware exception.
    EXPECT_FALSE(vmx::eventInjectionErrorCodeValid(
            /*isHardwareException=*/false, /*vector=*/13,
            /*deliverErrorCode=*/true));
    EXPECT_TRUE(vmx::eventInjectionErrorCodeValid(false, 13, false));
}

// SDM Vol. 3C 26.2.1.3: 0-15 is always in range, but 0 needs
// IA32_VMX_MISC[30].
TEST(VmxEventInjection, SoftwareClassInstructionLengthValid)
{
    EXPECT_TRUE(vmx::softwareClassInstructionLengthValid(1, false));
    EXPECT_TRUE(vmx::softwareClassInstructionLengthValid(15, false));
    EXPECT_FALSE(vmx::softwareClassInstructionLengthValid(16, false));
    EXPECT_FALSE(vmx::softwareClassInstructionLengthValid(0, false));
    EXPECT_TRUE(vmx::softwareClassInstructionLengthValid(0, true));
    EXPECT_FALSE(vmx::softwareClassInstructionLengthValid(16, true));
}

} // namespace
} // namespace X86ISA
} // namespace gem5
