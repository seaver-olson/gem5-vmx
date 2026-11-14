/*
 * Copyright (c) 2011 Google
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
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

#include "arch/x86/regs/msr.hh"

#include "arch/generic/mmu.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/paging.hh"
#include "cpu/thread_context.hh"

namespace gem5
{

namespace X86ISA
{

typedef MsrMap::value_type MsrVal;

const MsrMap::value_type msrMapData[] = {
    MsrVal(0x10, misc_reg::Tsc),
    MsrVal(0x1B, misc_reg::ApicBase),
    MsrVal(0x3A, misc_reg::FeatureControl), // IA32_FEATURE_CONTROL
    MsrVal(0xFE, misc_reg::Mtrrcap),
    MsrVal(0x174, misc_reg::SysenterCs),
    MsrVal(0x175, misc_reg::SysenterEsp),
    MsrVal(0x176, misc_reg::SysenterEip),
    MsrVal(0x179, misc_reg::McgCap),
    MsrVal(0x17A, misc_reg::McgStatus),
    MsrVal(0x17B, misc_reg::McgCtl),
    MsrVal(0x1D9, misc_reg::DebugCtlMsr),
    MsrVal(0x1DB, misc_reg::LastBranchFromIp),
    MsrVal(0x1DC, misc_reg::LastBranchToIp),
    MsrVal(0x1DD, misc_reg::LastExceptionFromIp),
    MsrVal(0x1DE, misc_reg::LastExceptionToIp),
    MsrVal(0x200, misc_reg::MtrrPhysBase0),
    MsrVal(0x201, misc_reg::MtrrPhysMask0),
    MsrVal(0x202, misc_reg::MtrrPhysBase1),
    MsrVal(0x203, misc_reg::MtrrPhysMask1),
    MsrVal(0x204, misc_reg::MtrrPhysBase2),
    MsrVal(0x205, misc_reg::MtrrPhysMask2),
    MsrVal(0x206, misc_reg::MtrrPhysBase3),
    MsrVal(0x207, misc_reg::MtrrPhysMask3),
    MsrVal(0x208, misc_reg::MtrrPhysBase4),
    MsrVal(0x209, misc_reg::MtrrPhysMask4),
    MsrVal(0x20A, misc_reg::MtrrPhysBase5),
    MsrVal(0x20B, misc_reg::MtrrPhysMask5),
    MsrVal(0x20C, misc_reg::MtrrPhysBase6),
    MsrVal(0x20D, misc_reg::MtrrPhysMask6),
    MsrVal(0x20E, misc_reg::MtrrPhysBase7),
    MsrVal(0x20F, misc_reg::MtrrPhysMask7),
    MsrVal(0x250, misc_reg::MtrrFix64k00000),
    MsrVal(0x258, misc_reg::MtrrFix16k80000),
    MsrVal(0x259, misc_reg::MtrrFix16kA0000),
    MsrVal(0x268, misc_reg::MtrrFix4kC0000),
    MsrVal(0x269, misc_reg::MtrrFix4kC8000),
    MsrVal(0x26A, misc_reg::MtrrFix4kD0000),
    MsrVal(0x26B, misc_reg::MtrrFix4kD8000),
    MsrVal(0x26C, misc_reg::MtrrFix4kE0000),
    MsrVal(0x26D, misc_reg::MtrrFix4kE8000),
    MsrVal(0x26E, misc_reg::MtrrFix4kF0000),
    MsrVal(0x26F, misc_reg::MtrrFix4kF8000),
    MsrVal(0x277, misc_reg::Pat),
    MsrVal(0x2FF, misc_reg::DefType),
    MsrVal(0x400, misc_reg::Mc0Ctl),
    MsrVal(0x404, misc_reg::Mc1Ctl),
    MsrVal(0x408, misc_reg::Mc2Ctl),
    MsrVal(0x40C, misc_reg::Mc3Ctl),
    MsrVal(0x410, misc_reg::Mc4Ctl),
    MsrVal(0x414, misc_reg::Mc5Ctl),
    MsrVal(0x418, misc_reg::Mc6Ctl),
    MsrVal(0x41C, misc_reg::Mc7Ctl),
    MsrVal(0x401, misc_reg::Mc0Status),
    MsrVal(0x405, misc_reg::Mc1Status),
    MsrVal(0x409, misc_reg::Mc2Status),
    MsrVal(0x40D, misc_reg::Mc3Status),
    MsrVal(0x411, misc_reg::Mc4Status),
    MsrVal(0x415, misc_reg::Mc5Status),
    MsrVal(0x419, misc_reg::Mc6Status),
    MsrVal(0x41D, misc_reg::Mc7Status),
    MsrVal(0x402, misc_reg::Mc0Addr),
    MsrVal(0x406, misc_reg::Mc1Addr),
    MsrVal(0x40A, misc_reg::Mc2Addr),
    MsrVal(0x40E, misc_reg::Mc3Addr),
    MsrVal(0x412, misc_reg::Mc4Addr),
    MsrVal(0x416, misc_reg::Mc5Addr),
    MsrVal(0x41A, misc_reg::Mc6Addr),
    MsrVal(0x41E, misc_reg::Mc7Addr),
    MsrVal(0x403, misc_reg::Mc0Misc),
    MsrVal(0x407, misc_reg::Mc1Misc),
    MsrVal(0x40B, misc_reg::Mc2Misc),
    MsrVal(0x40F, misc_reg::Mc3Misc),
    MsrVal(0x413, misc_reg::Mc4Misc),
    MsrVal(0x417, misc_reg::Mc5Misc),
    MsrVal(0x41B, misc_reg::Mc6Misc),
    MsrVal(0x41F, misc_reg::Mc7Misc),
    MsrVal(0x480, misc_reg::VmxBasic),          // IA32_VMX_BASIC
    MsrVal(0x481, misc_reg::VmxPinbasedCtls),   // IA32_VMX_PINBASED_CTLS
    MsrVal(0x482, misc_reg::VmxProcbasedCtls),  // IA32_VMX_PROCBASED_CTLS
    MsrVal(0x483, misc_reg::VmxExitCtls),       // IA32_VMX_EXIT_CTLS
    MsrVal(0x484, misc_reg::VmxEntryCtls),      // IA32_VMX_ENTRY_CTLS
    MsrVal(0x485, misc_reg::VmxMisc),           // IA32_VMX_MISC
    MsrVal(0x486, misc_reg::VmxCr0Fixed0),      // IA32_VMX_CR0_FIXED0
    MsrVal(0x487, misc_reg::VmxCr0Fixed1),      // IA32_VMX_CR0_FIXED1
    MsrVal(0x488, misc_reg::VmxCr4Fixed0),      // IA32_VMX_CR4_FIXED0
    MsrVal(0x489, misc_reg::VmxCr4Fixed1),      // IA32_VMX_CR4_FIXED1
    MsrVal(0x48A, misc_reg::VmxVmcsEnum),       // IA32_VMX_VMCS_ENUM
    MsrVal(0x48B, misc_reg::VmxProcbasedCtls2), // IA32_VMX_PROCBASED_CTLS2
    MsrVal(0x48C, misc_reg::VmxEptVpidCap),     // IA32_VMX_EPT_VPID_CAP
    // IA32_VMX_TRUE_PINBASED_CTLS / IA32_VMX_TRUE_PROCBASED_CTLS
    MsrVal(0x48D, misc_reg::VmxTruePinbasedCtls),
    MsrVal(0x48E, misc_reg::VmxTrueProcbasedCtls),
    MsrVal(0x48F, misc_reg::VmxTrueExitCtls),  // IA32_VMX_TRUE_EXIT_CTLS
    MsrVal(0x490, misc_reg::VmxTrueEntryCtls), // IA32_VMX_TRUE_ENTRY_CTLS
    MsrVal(0xC0000080, misc_reg::Efer),
    MsrVal(0xC0000081, misc_reg::Star),
    MsrVal(0xC0000082, misc_reg::Lstar),
    MsrVal(0xC0000083, misc_reg::Cstar),
    MsrVal(0xC0000084, misc_reg::SfMask),
    MsrVal(0xC0000100, misc_reg::FsBase),
    MsrVal(0xC0000101, misc_reg::GsBase),
    MsrVal(0xC0000102, misc_reg::KernelGsBase),
    MsrVal(0xC0000103, misc_reg::TscAux),
    MsrVal(0xC0010000, misc_reg::PerfEvtSel0),
    MsrVal(0xC0010001, misc_reg::PerfEvtSel1),
    MsrVal(0xC0010002, misc_reg::PerfEvtSel2),
    MsrVal(0xC0010003, misc_reg::PerfEvtSel3),
    MsrVal(0xC0010004, misc_reg::PerfEvtCtr0),
    MsrVal(0xC0010005, misc_reg::PerfEvtCtr1),
    MsrVal(0xC0010006, misc_reg::PerfEvtCtr2),
    MsrVal(0xC0010007, misc_reg::PerfEvtCtr3),
    MsrVal(0xC0010010, misc_reg::Syscfg),
    MsrVal(0xC0010016, misc_reg::IorrBase0),
    MsrVal(0xC0010017, misc_reg::IorrBase1),
    MsrVal(0xC0010018, misc_reg::IorrMask0),
    MsrVal(0xC0010019, misc_reg::IorrMask1),
    MsrVal(0xC001001A, misc_reg::TopMem),
    MsrVal(0xC001001D, misc_reg::TopMem2),
    MsrVal(0xC0010114, misc_reg::VmCr),
    MsrVal(0xC0010115, misc_reg::Ignne),
    MsrVal(0xC0010116, misc_reg::SmmCtl),
    MsrVal(0xC0010117, misc_reg::VmHsavePa),
};

static const unsigned msrMapSize = sizeof(msrMapData) / sizeof(msrMapData[0]);

const MsrMap msrMap(msrMapData, msrMapData + msrMapSize);

bool
msrAddrToIndex(RegIndex &reg_num, Addr addr)
{
    auto it = msrMap.find(addr);
    if (it == msrMap.end()) {
        return false;
    } else {
        reg_num = it->second;
        return true;
    }
}

bool
msrWritable(ThreadContext *tc, uint32_t address)
{
    RegIndex reg;
    if (!msrAddrToIndex(reg, address)) {
        return false;
    }
    // Capability registers are read-only, including the VMX capabilities.
    if (address == 0xfe || address == 0x179 ||
        (address >= 0x480 && address <= 0x490)) {
        return false;
    }
    return address != 0x3a ||
           !(tc->readMiscRegNoEffect(misc_reg::FeatureControl) & 1);
}

bool
msrWriteValid(ThreadContext *tc, uint32_t address, uint64_t value)
{
    if (!msrWritable(tc, address)) {
        return false;
    }
    const auto mtrr_type = [](uint8_t type) {
        return type == 0 || type == 1 || (type >= 4 && type <= 6);
    };
    if (address >= 0x200 && address <= 0x20f) {
        CpuidResult sizes;
        auto *isa = static_cast<ISA *>(tc->getIsaPtr());
        isa->cpuid->doCpuid(tc, LongModeAddressSize, 0, sizes);
        const unsigned width = bits(sizes.rax, 7, 0);
        if (width < 32 || width > 52) {
            return false;
        }
        const uint64_t addresses = mask(width) & ~mask(12);
        if (address & 1) {
            if (value & ~(addresses | (1ULL << 11))) {
                return false;
            }
        } else if ((value & ~(addresses | mask(8))) ||
                   !mtrr_type(uint8_t(value))) {
            return false;
        }
    }
    if (address == 0x250 || address == 0x258 || address == 0x259 ||
        (address >= 0x268 && address <= 0x26f)) {
        for (unsigned i = 0; i < 8; ++i) {
            if (!mtrr_type(uint8_t(value >> (8 * i)))) {
                return false;
            }
        }
    }
    switch (address) {
        case 0x2ff: // IA32_MTRR_DEF_TYPE
            if ((value & ~0xc07ULL) || !mtrr_type(uint8_t(value))) {
                return false;
            }
            break;
        case 0x1d9: // IA32_DEBUGCTL reserved positions, SDM vol. 4 Table 2-2.
            if (value & (~mask(16) | mask(5, 3))) {
                return false;
            }
            break;
        case 0x277: // IA32_PAT
            for (unsigned i = 0; i < 8; ++i) {
                if (!paging::validPatType(uint8_t(value >> (8 * i)))) {
                    return false;
                }
            }
            break;
        case 0xc0000080: { // IA32_EFER; LMA is read-only, not reserved.
            const uint64_t old = tc->readMiscRegNoEffect(misc_reg::Efer);
            if (value & ~0xd01ULL) {
                return false;
            }
            if (bits(tc->readMiscRegNoEffect(misc_reg::Cr0), 31) &&
                ((old ^ value) & (1ULL << 8))) {
                return false;
            }
            break;
        }
        case 0x175:      // IA32_SYSENTER_ESP
        case 0x176:      // IA32_SYSENTER_EIP
        case 0xc0000082: // IA32_LSTAR
        case 0xc0000100: // IA32_FS_BASE
        case 0xc0000101: // IA32_GS_BASE
        case 0xc0000102: // IA32_KERNEL_GS_BASE
            if ((value >> 47) != 0 && (value >> 47) != mask(17)) {
                return false;
            }
            break;
        case 0xc0000103: // IA32_TSC_AUX
            if (value >> 32) {
                return false;
            }
            break;
        default:
            break;
    }
    return true;
}

bool
readMsr(ThreadContext *tc, uint32_t address, uint64_t &value)
{
    RegIndex reg;
    if (!msrAddrToIndex(reg, address)) {
        return false;
    }
    value = tc->readMiscReg(reg);
    return true;
}

bool
writeMsr(ThreadContext *tc, uint32_t address, uint64_t value)
{
    if (!msrWriteValid(tc, address, value)) {
        return false;
    }
    RegIndex reg;
    msrAddrToIndex(reg, address);
    if (reg == misc_reg::Efer) {
        // WRMSR cannot modify LMA, including when used by a VMX MSR list.
        value = insertBits(value, 10,
                           bits(tc->readMiscRegNoEffect(misc_reg::Efer), 10));
    }
    tc->setMiscReg(reg, value);
    // SDM WRMSR and 29.4/30.6: MTRR writes invalidate all TLB entries,
    // including globals, whether explicit or performed by a VMX list.
    if ((address >= 0x200 && address <= 0x20f) || address == 0x250 ||
        address == 0x258 || address == 0x259 ||
        (address >= 0x268 && address <= 0x26f) || address == 0x2ff) {
        tc->getMMUPtr()->flushAll();
    }
    return true;
}

} // namespace X86ISA
} // namespace gem5
