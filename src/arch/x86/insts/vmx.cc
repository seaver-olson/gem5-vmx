#include "arch/x86/insts/vmx.hh"

#include <array>
#include <memory>
#include <string> // for std::to_string
#include <utility> // for std::pair and std::move
#include <vector>

#include "arch/x86/decoder.hh"
#include "arch/x86/faults.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/ldstflags.hh"
#include "arch/x86/page_size.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/utility.hh"
#include "arch/x86/vmx_utils.hh"
#include "base/bitfield.hh"
#include "cpu/exec_context.hh"
#include "cpu/thread_context.hh"
#include "debug/VMX.hh"
#include "mem/port_proxy.hh"
#include "sim/system.hh"

namespace gem5
{
namespace X86ISA
{

Fault
vmxMemoryOperandFault(ThreadContext *tc, Addr linear,
        size_t size, Request::Flags operandFlags)
{
    HandyM5Reg mode = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    if (mode.mode == LongMode) {
        const auto canonical = [](Addr address) {
            const uint64_t high = bits(address, 63, 48);
            return high == (bits(address, 47) ? mask(16) : 0);
        };
        const bool wraps = size && linear > MaxAddr - (size - 1);
        const Addr last = size ? linear + size - 1 : linear;
        if (wraps || !canonical(linear) || !canonical(last)) {
            const int segment = operandFlags & SegmentFlagMask;
            return segment == segment_idx::Ss ?
                Fault(std::make_shared<StackFault>(0)) :
                Fault(std::make_shared<GeneralProtection>(0));
        }
    }
    return NoFault;
}

namespace
{
constexpr uint32_t
toInt(VmxExitReason reason)
{
    return static_cast<uint32_t>(reason);
}

constexpr uint32_t
toInt(VmxInstructionError error)
{
    return static_cast<uint32_t>(error);
}

using VmcsField = Vmcs::Field;

constexpr VmcsField VmcsVmExitReason = VmcsField::VmExitReason;
constexpr VmcsField VmcsVmExitInterruptionInfo =
    VmcsField::VmExitInterruptionInfo;
constexpr VmcsField VmcsVmExitInterruptionErrorCode =
    VmcsField::VmExitInterruptionErrorCode;
constexpr VmcsField VmcsVmExitInstructionLen =
    VmcsField::VmExitInstructionLen;
constexpr VmcsField VmcsVmxInstructionInfo = VmcsField::VmxInstructionInfo;
constexpr VmcsField VmcsPinBasedVmExecControl =
    VmcsField::PinBasedVmExecControl;
constexpr VmcsField VmcsCpuBasedVmExecControl =
    VmcsField::CpuBasedVmExecControl;
constexpr VmcsField VmcsExceptionBitmap = VmcsField::ExceptionBitmap;
constexpr VmcsField VmcsPageFaultErrorCodeMask =
    VmcsField::PageFaultErrorCodeMask;
constexpr VmcsField VmcsPageFaultErrorCodeMatch =
    VmcsField::PageFaultErrorCodeMatch;
constexpr VmcsField VmcsIoBitmapA = VmcsField::IoBitmapA;
constexpr VmcsField VmcsIoBitmapB = VmcsField::IoBitmapB;
constexpr VmcsField VmcsMsrBitmap = VmcsField::MsrBitmap;
constexpr VmcsField VmcsGuestPhysicalAddress =
    VmcsField::GuestPhysicalAddress;
constexpr VmcsField VmcsCr0GuestHostMask = VmcsField::Cr0GuestHostMask;
constexpr VmcsField VmcsCr4GuestHostMask = VmcsField::Cr4GuestHostMask;
constexpr VmcsField VmcsCr0ReadShadow = VmcsField::Cr0ReadShadow;
constexpr VmcsField VmcsCr4ReadShadow = VmcsField::Cr4ReadShadow;
constexpr VmcsField VmcsExitQualification = VmcsField::ExitQualification;
constexpr VmcsField VmcsGuestLinearAddress = VmcsField::GuestLinearAddress;
constexpr VmcsField VmcsGuestRip = VmcsField::GuestRip;
constexpr VmcsField VmcsGuestRsp = VmcsField::GuestRsp;
constexpr VmcsField VmcsGuestRflags = VmcsField::GuestRflags;
constexpr VmcsField VmcsGuestCr0 = VmcsField::GuestCr0;
constexpr VmcsField VmcsGuestCr3 = VmcsField::GuestCr3;
constexpr VmcsField VmcsGuestCr4 = VmcsField::GuestCr4;
constexpr VmcsField VmcsGuestDr7 = VmcsField::GuestDr7;
constexpr VmcsField VmcsGuestIa32Efer = VmcsField::GuestIa32Efer;
constexpr VmcsField VmcsGuestActivityState = VmcsField::GuestActivityState;
constexpr VmcsField VmcsHostRip = VmcsField::HostRip;
constexpr VmcsField VmcsHostRsp = VmcsField::HostRsp;
constexpr VmcsField VmcsHostCr0 = VmcsField::HostCr0;
constexpr VmcsField VmcsHostCr3 = VmcsField::HostCr3;
constexpr VmcsField VmcsHostCr4 = VmcsField::HostCr4;
constexpr VmcsField VmcsHostIa32Efer = VmcsField::HostIa32Efer;
constexpr VmcsField VmcsVmEntryControls = VmcsField::VmEntryControls;
constexpr VmcsField VmcsVmEntryIntrInfoField = VmcsField::VmEntryIntrInfoField;
constexpr VmcsField VmcsVmEntryMsrLoadCount = VmcsField::VmEntryMsrLoadCount;
constexpr VmcsField VmcsVmExitControls = VmcsField::VmExitControls;
constexpr VmcsField VmcsVmExitMsrStoreCount = VmcsField::VmExitMsrStoreCount;
constexpr VmcsField VmcsVmExitMsrLoadCount = VmcsField::VmExitMsrLoadCount;
constexpr VmcsField VmcsSecondaryVmExecControl =
    VmcsField::SecondaryVmExecControl;
constexpr VmcsField VmcsCr3TargetCount = VmcsField::Cr3TargetCount;
constexpr VmcsField VmcsGuestIa32SysenterCs = VmcsField::GuestSysenterCs;
constexpr VmcsField VmcsGuestIa32SysenterEsp = VmcsField::GuestSysenterEsp;
constexpr VmcsField VmcsGuestIa32SysenterEip = VmcsField::GuestSysenterEip;
constexpr VmcsField VmcsHostIa32SysenterCs = VmcsField::HostIa32SysenterCs;
constexpr VmcsField VmcsHostIa32SysenterEsp = VmcsField::HostIa32SysenterEsp;
constexpr VmcsField VmcsHostIa32SysenterEip = VmcsField::HostIa32SysenterEip;

constexpr uint32_t PinBasedExternalInterruptExiting = 1u << 0;
constexpr uint32_t PinBasedNmiExiting = 1u << 3;

constexpr uint32_t CpuBasedHltExiting = 1u << 7;
constexpr uint32_t CpuBasedInvlpgExiting = 1u << 9;
constexpr uint32_t CpuBasedCr3LoadExiting = 1u << 15;
constexpr uint32_t CpuBasedCr3StoreExiting = 1u << 16;
constexpr uint32_t CpuBasedCr8LoadExiting = 1u << 19;
constexpr uint32_t CpuBasedCr8StoreExiting = 1u << 20;
constexpr uint32_t CpuBasedMovDrExiting = 1u << 23;
constexpr uint32_t CpuBasedUnconditionalIoExiting = 1u << 24;
constexpr uint32_t CpuBasedUseIoBitmaps = 1u << 25;
constexpr uint32_t CpuBasedUseMsrBitmaps = 1u << 28;
constexpr uint32_t CpuBasedActivateSecondaryControls = 1u << 31;

constexpr uint32_t VmExitHostAddressSpaceSize = 1u << 9;
constexpr uint32_t VmExitSaveDebugControls = 1u << 2;
constexpr uint32_t VmExitSaveIa32Efer = 1u << 20;
constexpr uint32_t VmExitLoadIa32Efer = 1u << 21;

constexpr uint32_t VmEntryIa32eModeGuest = 1u << 9;
constexpr uint32_t VmEntryLoadIa32Efer = 1u << 15;

constexpr uint32_t VmExitReasonVmEntryFailure = 1u << 31;
constexpr uint64_t RequiredRflagsBit = 1ull << 1;
constexpr uint64_t ReservedRflagsMask =
    mask(63, 22) | (1ull << 15) | (1ull << 5) | (1ull << 3);
constexpr uint64_t SupportedEferBits =
    (1ull << 0) | (1ull << 8) | (1ull << 10) | (1ull << 11);

struct SegmentFieldSet
{
    int index;
    VmcsField selector;
    VmcsField base;
    VmcsField limit;
    VmcsField attr;
};

static constexpr std::array<SegmentFieldSet, 8> GuestSegments = {{
    {segment_idx::Es, VmcsField::GuestEsSelector, VmcsField::GuestEsBase,
        VmcsField::GuestEsLimit, VmcsField::GuestEsAccessRights},
    {segment_idx::Cs, VmcsField::GuestCsSelector, VmcsField::GuestCsBase,
        VmcsField::GuestCsLimit, VmcsField::GuestCsAccessRights},
    {segment_idx::Ss, VmcsField::GuestSsSelector, VmcsField::GuestSsBase,
        VmcsField::GuestSsLimit, VmcsField::GuestSsAccessRights},
    {segment_idx::Ds, VmcsField::GuestDsSelector, VmcsField::GuestDsBase,
        VmcsField::GuestDsLimit, VmcsField::GuestDsAccessRights},
    {segment_idx::Fs, VmcsField::GuestFsSelector, VmcsField::GuestFsBase,
        VmcsField::GuestFsLimit, VmcsField::GuestFsAccessRights},
    {segment_idx::Gs, VmcsField::GuestGsSelector, VmcsField::GuestGsBase,
        VmcsField::GuestGsLimit, VmcsField::GuestGsAccessRights},
    {segment_idx::Tsl, VmcsField::GuestLdtrSelector,
        VmcsField::GuestLdtrBase, VmcsField::GuestLdtrLimit,
        VmcsField::GuestLdtrAccessRights},
    {segment_idx::Tr, VmcsField::GuestTrSelector, VmcsField::GuestTrBase,
        VmcsField::GuestTrLimit, VmcsField::GuestTrAccessRights},
}};

struct HostSelectorField
{
    int index;
    VmcsField selector;
};

static constexpr std::array<HostSelectorField, 7> HostSelectors = {{
    {segment_idx::Es, VmcsField::HostEsSelector},
    {segment_idx::Cs, VmcsField::HostCsSelector},
    {segment_idx::Ss, VmcsField::HostSsSelector},
    {segment_idx::Ds, VmcsField::HostDsSelector},
    {segment_idx::Fs, VmcsField::HostFsSelector},
    {segment_idx::Gs, VmcsField::HostGsSelector},
    {segment_idx::Tr, VmcsField::HostTrSelector},
}};

uint32_t
vmcsRevisionId(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    return bits(isa->readMiscRegNoEffect(misc_reg::VmxBasic), 30, 0);
}

Fault
readOperand(ExecContext *xc, Addr operandEA, Request::Flags operandFlags,
        uint64_t &value, size_t size)
{
    if (auto fault = vmxMemoryOperandFault(
                xc->tcBase(), operandEA, size, operandFlags);
            fault != NoFault) {
        return fault;
    }
    const std::vector<bool> byteEnable(size, true);
    value = 0;
    auto fault = xc->readMem(
            operandEA, reinterpret_cast<uint8_t *>(&value), size,
            operandFlags, byteEnable);
    return fault;
}

bool
readVmcsHeader(ThreadContext *tc, Addr regionPtr, Vmcs::VmcsHeader &header)
{
    PortProxy proxy(tc, tc->getSystemPtr()->cacheLineSize());
    proxy.readBlob(regionPtr, &header, sizeof(header));
    return true;
}

bool
validPhysicalAddress(Addr addr)
{
    return vmx::validPhysicalAddress(addr, 48);
}

bool
validAlignedPhysicalAddress(Addr addr, size_t alignment)
{
    return validPhysicalAddress(addr) && (addr & (alignment - 1)) == 0;
}

bool
canonicalAddress(Addr addr)
{
    const uint64_t high = bits(addr, 63, 48);
    return high == (bits(addr, 47) ? mask(16) : 0);
}

bool
uniformUpperLinearBits(Addr addr)
{
    const uint64_t high = bits(addr, 63, 48);
    return high == 0 || high == mask(16);
}

bool
readRequired(const Vmcs &vmcs, VmcsField field, uint64_t &value)
{
    return vmcs.read(field, value);
}

uint64_t
readOrZero(const Vmcs &vmcs, VmcsField field)
{
    uint64_t value = 0;
    vmcs.read(field, value);
    return value;
}

uint64_t
vmxControlCapability(ThreadContext *tc, RegIndex legacyMsr,
        RegIndex trueMsr)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    const uint64_t basic = isa->readMiscRegNoEffect(misc_reg::VmxBasic);
    return bits(basic, 55) ? isa->readMiscRegNoEffect(trueMsr) :
        isa->readMiscRegNoEffect(legacyMsr);
}

bool
vmxCr4Enabled(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    CR4 cr4 = isa->readMiscRegNoEffect(misc_reg::Cr4);
    return cr4.vmxe;
}

bool
vmxInstructionRecognized(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    CR0 cr0 = isa->readMiscRegNoEffect(misc_reg::Cr0);
    Efer efer = isa->readMiscRegNoEffect(misc_reg::Efer);
    SegAttr cs = isa->readMiscRegNoEffect(misc_reg::CsAttr);
    return cr0.pe && !(getRFlags(tc) & VMBit) &&
        !(efer.lma && !cs.longMode);
}

bool
atCpl0(ThreadContext *tc)
{
    HandyM5Reg m5Reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    return m5Reg.cpl == 0;
}

bool
vmxAvailable(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    const RegVal featureControl =
        isa->readMiscRegNoEffect(misc_reg::FeatureControl);

    const bool featureLocked = bits(featureControl, 0);
    const bool vmxonEnabled = bits(featureControl, 2);
    const uint64_t cr0 = isa->readMiscRegNoEffect(misc_reg::Cr0);
    const uint64_t cr4 = isa->readMiscRegNoEffect(misc_reg::Cr4);
    return featureLocked && vmxonEnabled &&
        vmx::fixedBitsAllowed(cr0,
            isa->readMiscRegNoEffect(misc_reg::VmxCr0Fixed0),
            isa->readMiscRegNoEffect(misc_reg::VmxCr0Fixed1)) &&
        vmx::fixedBitsAllowed(cr4,
            isa->readMiscRegNoEffect(misc_reg::VmxCr4Fixed0),
            isa->readMiscRegNoEffect(misc_reg::VmxCr4Fixed1));
}

bool
validateRegion(ThreadContext *tc, Addr regionPtr)
{
    if (!validAlignedPhysicalAddress(regionPtr, Vmcs::VmcsRegionSize)) {
        return false;
    }

    Vmcs::VmcsHeader header;
    readVmcsHeader(tc, regionPtr, header);

    return bits(header.revisionId, 30, 0) == vmcsRevisionId(tc) &&
           !bits(header.revisionId, 31);
}

VmxResult
vmFailValid(Vmcs *vmcs, uint32_t error)
{
    panic_if(!vmcs, "VMfailValid requires a current VMCS");
    vmcs->setInstructionError(error);
    return VmxResult::failValid(error);
}

VmxResult
vmFailIfCurrent(Vmcs *vmcs, VmxInstructionError error)
{
    return vmcs ? vmFailValid(vmcs, toInt(error)) :
        VmxResult::failInvalid();
}

SegAttr
vmcsAccessRightsToSegAttr(uint32_t accessRights)
{
    SegAttr attr = 0;
    attr.type = bits(accessRights, 3, 0);
    attr.system = bits(accessRights, 4);
    attr.dpl = bits(accessRights, 6, 5);
    attr.present = bits(accessRights, 7);
    attr.avl = bits(accessRights, 12);
    attr.longMode = bits(accessRights, 13);
    attr.defaultSize = bits(accessRights, 14);
    attr.granularity = bits(accessRights, 15);
    attr.unusable = bits(accessRights, 16);

    if (attr.system) {
        const uint32_t type = attr.type;
        const bool code = type & 0x8;
        attr.readable = code ? bits(type, 1) : 1;
        attr.writable = code ? 0 : bits(type, 1);
        attr.expandDown = code ? 0 : bits(type, 2);
    } else {
        attr.readable = 1;
        attr.writable = 1;
        attr.expandDown = 0;
    }

    return attr;
}

uint32_t
segAttrToVmcsAccessRights(SegAttr attr)
{
    uint32_t accessRights = 0;
    replaceBits(accessRights, 3, 0, static_cast<uint32_t>(attr.type));
    replaceBits(accessRights, 4, static_cast<uint32_t>(attr.system));
    replaceBits(accessRights, 6, 5, static_cast<uint32_t>(attr.dpl));
    replaceBits(accessRights, 7, static_cast<uint32_t>(attr.present));
    replaceBits(accessRights, 12, static_cast<uint32_t>(attr.avl));
    replaceBits(accessRights, 13, static_cast<uint32_t>(attr.longMode));
    replaceBits(accessRights, 14, static_cast<uint32_t>(attr.defaultSize));
    replaceBits(accessRights, 15, static_cast<uint32_t>(attr.granularity));
    replaceBits(accessRights, 16, static_cast<uint32_t>(attr.unusable));
    return accessRights;
}

bool
validSegmentLimit(uint32_t limit, SegAttr attr)
{
    if (attr.granularity) {
        return bits(limit, 11, 0) == mask(12);
    }
    return bits(limit, 31, 20) == 0;
}

bool
validGuestSegment(int index, uint16_t selector, uint64_t base,
        uint32_t limit, uint32_t accessRights, bool ia32e, uint8_t cpl)
{
    const SegAttr attr = vmcsAccessRightsToSegAttr(accessRights);
    if ((index == segment_idx::Cs || index == segment_idx::Tr) &&
            attr.unusable) {
        return false;
    }
    // On an Intel-64-capable processor these base checks are per register,
    // not a generic IA-32e/usable rule (SDM 29.3.1.2). FS and GS must be
    // canonical even when unusable; a usable LDTR and TR must also be
    // canonical. CS and usable SS/DS/ES instead require zero upper 32 bits.
    const bool canonicalBase = index == segment_idx::Fs ||
        index == segment_idx::Gs || index == segment_idx::Tr ||
        (index == segment_idx::Tsl && !attr.unusable);
    const bool base32 = index == segment_idx::Cs ||
        ((index == segment_idx::Ss || index == segment_idx::Ds ||
          index == segment_idx::Es) && !attr.unusable);
    if ((canonicalBase && !canonicalAddress(base)) ||
            (base32 && bits(base, 63, 32) != 0)) {
        return false;
    }
    if (attr.unusable) {
        return true;
    }

    // For ordinary unusable segments Intel does not check these reserved
    // positions. CS and TR cannot be unusable and therefore reach this check.
    // Bit 16 is the VMX-specific unusable bit; bit 12 is software available.
    if (accessRights & (mask(31, 17) | mask(11, 8))) {
        return false;
    }
    if (!attr.present || !validSegmentLimit(limit, attr)) {
        return false;
    }

    const uint8_t type = attr.type;
    const uint8_t rpl = bits(selector, 1, 0);
    switch (index) {
      case segment_idx::Cs:
        if (!attr.system || !(type & 0x8) || !(type & 0x1) ||
                (attr.longMode && attr.defaultSize) ||
                (!ia32e && attr.longMode)) {
            return false;
        }
        // Conforming code may have a lower DPL; nonconforming code must
        // match the guest CPL exactly.
        return (type & 0x4) ? attr.dpl <= rpl : attr.dpl == rpl;

      case segment_idx::Ss:
        return attr.system && !(type & 0x8) && (type & 0x3) == 0x3 &&
            attr.dpl == cpl && rpl == cpl;

      case segment_idx::Es:
      case segment_idx::Ds:
      case segment_idx::Fs:
      case segment_idx::Gs:
        if (!attr.system || !(type & 0x1) ||
                ((type & 0x8) && !(type & 0x2))) {
            return false;
        }
        if (!(type & 0x8) || !(type & 0x4)) {
            return attr.dpl >= rpl;
        }
        return true;

      case segment_idx::Tsl:
        return !attr.system && type == 0x2 && !bits(selector, 2);

      case segment_idx::Tr:
        return !attr.system && (type == 0x3 || type == 0xb) &&
            (!ia32e || type == 0xb) && !bits(selector, 2);

      default:
        return false;
    }
}

SegAttr
hostCodeAttr(bool longMode)
{
    SegAttr attr = 0;
    attr.type = 0xb;
    attr.system = 1;
    attr.present = 1;
    attr.readable = 1;
    attr.longMode = longMode;
    attr.defaultSize = longMode ? 0 : 1;
    attr.granularity = 1;
    return attr;
}

SegAttr
hostDataAttr()
{
    SegAttr attr = 0;
    attr.type = 0x3;
    attr.system = 1;
    attr.present = 1;
    attr.readable = 1;
    attr.writable = 1;
    attr.defaultSize = 1;
    attr.granularity = 1;
    return attr;
}

SegAttr
hostTssAttr()
{
    SegAttr attr = 0;
    attr.type = 0xb;
    attr.present = 1;
    attr.readable = 1;
    attr.writable = 1;
    return attr;
}

Addr
currentRip(ThreadContext *tc)
{
    const auto &pc = tc->pcState().as<PCState>();
    const Addr csBase = tc->readMiscRegNoEffect(misc_reg::CsEffBase);
    return pc.instAddr() - csBase;
}

void
redirectNextPc(ThreadContext *tc, Addr nextPc)
{
    PCState pc = tc->pcState().as<PCState>();
    pc.setNPC(nextPc);
    tc->pcState(pc);
}

void
redirectNow(ThreadContext *tc, Addr nextPc)
{
    PCState pc(nextPc);
    tc->pcState(pc);
}

uint64_t
ioQualification(bool read, uint16_t port, size_t size)
{
    uint64_t qualification = 0;

    switch (size) {
      case 1:
        qualification |= 0;
        break;
      case 2:
        qualification |= 1;
        break;
      case 4:
        qualification |= 3;
        break;
      default:
        panic("Unsupported VMX I/O exit size %zu", size);
    }

    if (read) {
        qualification |= 1ull << 3;
    }
    qualification |= static_cast<uint64_t>(port) << 16;
    return qualification;
}

uint64_t
crQualification(uint8_t cr, VmxCrAccessType type, uint8_t gpr,
        uint64_t value, bool lmswMemoryOperand)
{
    uint64_t qualification = cr & 0xf;
    qualification |= (static_cast<uint64_t>(type) & 0x3) << 4;
    qualification |= (static_cast<uint64_t>(gpr) & 0xf) << 8;
    if (type == VmxCrAccessType::Lmsw) {
        qualification |= vmx::lmswQualificationFields(
                value, lmswMemoryOperand);
    }
    return qualification;
}

uint64_t
drQualification(uint8_t dr, bool fromDr, uint8_t gpr)
{
    uint64_t qualification = dr & 0xf;
    if (fromDr) {
        qualification |= 1ull << 4;
    }
    qualification |= (static_cast<uint64_t>(gpr) & 0xf) << 8;
    return qualification;
}

bool
isLowOrHighMsr(uint32_t msr, uint32_t &index, bool &high)
{
    if (msr <= 0x1fff) {
        index = msr;
        high = false;
        return true;
    }

    if (msr >= 0xc0000000 && msr <= 0xc0001fff) {
        index = msr & 0x1fff;
        high = true;
        return true;
    }

    return false;
}

} // namespace

void
VmxExitFault::invoke(ThreadContext *tc, const StaticInstPtr &inst)
{
    auto *isa = dynamic_cast<ISA *>(tc->getIsaPtr());
    VmxExitInfo info = exitInfo;
    if (info.hasInstructionLength && info.instructionLength == 0 && inst) {
        info.instructionLength = inst->size();
    }
    if (!isa || !isa->vmxState().vmexitEvent(tc, info)) {
        panic("Unable to complete VMX exit fault");
    }
}

VmxState::VmEntryValidationResult
VmxState::validateVmEntry(ThreadContext *tc, Vmcs &vmcs) const
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    auto failInstruction = [](VmxInstructionError error) {
        VmEntryValidationResult result;
        result.valid = false;
        result.vmEntryFailure = false;
        result.instructionError = error;
        return result;
    };
    auto failEntry = [](VmxExitReason reason) {
        VmEntryValidationResult result;
        result.valid = false;
        result.vmEntryFailure = true;
        result.exitReason = reason;
        return result;
    };

    uint64_t pinControls = 0;
    uint64_t procControls = 0;
    uint64_t exitControls = 0;
    uint64_t entryControls = 0;

    if (!readRequired(vmcs, VmcsPinBasedVmExecControl, pinControls) ||
            !readRequired(vmcs, VmcsCpuBasedVmExecControl, procControls) ||
            !readRequired(vmcs, VmcsVmExitControls, exitControls) ||
            !readRequired(vmcs, VmcsVmEntryControls, entryControls)) {
        return failInstruction(
                VmxInstructionError::VmEntryInvalidControlFields);
    }

    if (!vmx::controlsAllowed(bits(pinControls, 31, 0),
                vmxControlCapability(tc, misc_reg::VmxPinbasedCtls,
                    misc_reg::VmxTruePinbasedCtls)) ||
            !vmx::controlsAllowed(bits(procControls, 31, 0),
                vmxControlCapability(tc, misc_reg::VmxProcbasedCtls,
                    misc_reg::VmxTrueProcbasedCtls)) ||
            !vmx::controlsAllowed(bits(exitControls, 31, 0),
                vmxControlCapability(tc, misc_reg::VmxExitCtls,
                    misc_reg::VmxTrueExitCtls)) ||
            !vmx::controlsAllowed(bits(entryControls, 31, 0),
                vmxControlCapability(tc, misc_reg::VmxEntryCtls,
                    misc_reg::VmxTrueEntryCtls))) {
        return failInstruction(
                VmxInstructionError::VmEntryInvalidControlFields);
    }

    if (procControls & CpuBasedActivateSecondaryControls) {
        uint64_t secondaryControls = 0;
        if (!readRequired(vmcs, VmcsSecondaryVmExecControl,
                    secondaryControls) ||
                !vmx::controlsAllowed(bits(secondaryControls, 31, 0),
                    isa->readMiscRegNoEffect(misc_reg::VmxProcbasedCtls2))) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidControlFields);
        }
    }

    const uint64_t cr3TargetCount = readOrZero(vmcs, VmcsCr3TargetCount);
    if (cr3TargetCount != 0 ||
            readOrZero(vmcs, VmcsVmExitMsrStoreCount) != 0 ||
            readOrZero(vmcs, VmcsVmExitMsrLoadCount) != 0 ||
            readOrZero(vmcs, VmcsVmEntryMsrLoadCount) != 0 ||
            readOrZero(vmcs, VmcsVmEntryIntrInfoField) != 0) {
        return failInstruction(
                VmxInstructionError::VmEntryInvalidControlFields);
    }

    if ((procControls & CpuBasedUseIoBitmaps) &&
            (!validAlignedPhysicalAddress(readOrZero(vmcs, VmcsIoBitmapA),
                 PageBytes) ||
             !validAlignedPhysicalAddress(readOrZero(vmcs, VmcsIoBitmapB),
                 PageBytes))) {
        return failInstruction(
                VmxInstructionError::VmEntryInvalidControlFields);
    }

    if ((procControls & CpuBasedUseMsrBitmaps) &&
            !validAlignedPhysicalAddress(readOrZero(vmcs, VmcsMsrBitmap),
                PageBytes)) {
        return failInstruction(
                VmxInstructionError::VmEntryInvalidControlFields);
    }

    uint64_t hostCr0 = 0;
    uint64_t hostCr3 = 0;
    uint64_t hostCr4 = 0;
    uint64_t hostRsp = 0;
    uint64_t hostRip = 0;
    if (!readRequired(vmcs, VmcsHostCr0, hostCr0) ||
            !readRequired(vmcs, VmcsHostCr3, hostCr3) ||
            !readRequired(vmcs, VmcsHostCr4, hostCr4) ||
            !readRequired(vmcs, VmcsHostRsp, hostRsp) ||
            !readRequired(vmcs, VmcsHostRip, hostRip)) {
        return failInstruction(VmxInstructionError::VmEntryInvalidHostState);
    }

    CR0 hostCr0Bits = hostCr0;
    CR4 hostCr4Bits = hostCr4;
    const bool hostIa32e = exitControls & VmExitHostAddressSpaceSize;
    const bool guestIa32e = entryControls & VmEntryIa32eModeGuest;
    Efer currentEfer = tc->readMiscRegNoEffect(misc_reg::Efer);
    constexpr uint64_t ignoredCr0Bits = (1ull << 29) | (1ull << 30);
    if (!vmx::fixedBitsAllowed(hostCr0,
                isa->readMiscRegNoEffect(misc_reg::VmxCr0Fixed0),
                isa->readMiscRegNoEffect(misc_reg::VmxCr0Fixed1),
                ignoredCr0Bits) ||
            !vmx::fixedBitsAllowed(hostCr4,
                isa->readMiscRegNoEffect(misc_reg::VmxCr4Fixed0),
                isa->readMiscRegNoEffect(misc_reg::VmxCr4Fixed1)) ||
            !hostCr0Bits.pe || !hostCr4Bits.vmxe ||
            (currentEfer.lma ? !hostIa32e :
                (hostIa32e || guestIa32e)) ||
            (hostIa32e && (!hostCr0Bits.pg || !hostCr4Bits.pae)) ||
            !vmx::validCr3(hostCr3, hostCr4Bits, hostIa32e, 48) ||
            (hostIa32e ? !canonicalAddress(hostRip) :
                bits(hostRip, 63, 32) != 0) ||
            !canonicalAddress(readOrZero(vmcs, VmcsHostIa32SysenterEsp)) ||
            !canonicalAddress(readOrZero(vmcs,
                    VmcsHostIa32SysenterEip))) {
        return failInstruction(VmxInstructionError::VmEntryInvalidHostState);
    }

    if (exitControls & VmExitLoadIa32Efer) {
        const uint64_t hostEferValue =
            readOrZero(vmcs, VmcsHostIa32Efer);
        Efer hostEfer = hostEferValue;
        if ((hostEferValue & ~SupportedEferBits) ||
                hostEfer.lma != hostIa32e ||
                hostEfer.lme != hostIa32e) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidHostState);
        }
    }

    for (const auto &hostSelector : HostSelectors) {
        uint64_t selector = 0;
        if (!readRequired(vmcs, hostSelector.selector, selector)) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidHostState);
        }
        if ((hostSelector.index == segment_idx::Cs ||
             hostSelector.index == segment_idx::Tr) && selector == 0) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidHostState);
        }
        if (bits(selector, 2, 0) != 0) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidHostState);
        }
        if (hostSelector.index == segment_idx::Ss && !hostIa32e &&
                selector == 0) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidHostState);
        }
    }

    for (const auto field : {VmcsField::HostFsBase,
             VmcsField::HostGsBase, VmcsField::HostTrBase,
             VmcsField::HostGdtrBase, VmcsField::HostIdtrBase}) {
        if (!canonicalAddress(readOrZero(vmcs, field))) {
            return failInstruction(
                    VmxInstructionError::VmEntryInvalidHostState);
        }
    }

    uint64_t guestCr0 = 0;
    uint64_t guestCr3 = 0;
    uint64_t guestCr4 = 0;
    uint64_t guestRsp = 0;
    uint64_t guestRip = 0;
    uint64_t guestRflags = 0;
    if (!readRequired(vmcs, VmcsGuestCr0, guestCr0) ||
            !readRequired(vmcs, VmcsGuestCr3, guestCr3) ||
            !readRequired(vmcs, VmcsGuestCr4, guestCr4) ||
            !readRequired(vmcs, VmcsGuestRsp, guestRsp) ||
            !readRequired(vmcs, VmcsGuestRip, guestRip) ||
            !readRequired(vmcs, VmcsGuestRflags, guestRflags)) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }

    CR0 guestCr0Bits = guestCr0;
    CR4 guestCr4Bits = guestCr4;
    if (!vmx::fixedBitsAllowed(guestCr0,
                isa->readMiscRegNoEffect(misc_reg::VmxCr0Fixed0),
                isa->readMiscRegNoEffect(misc_reg::VmxCr0Fixed1),
                ignoredCr0Bits) ||
            !vmx::fixedBitsAllowed(guestCr4,
                isa->readMiscRegNoEffect(misc_reg::VmxCr4Fixed0),
                isa->readMiscRegNoEffect(misc_reg::VmxCr4Fixed1)) ||
            !guestCr0Bits.pe ||
            !vmx::validCr3(guestCr3, guestCr4Bits, guestIa32e, 48) ||
            !(guestRflags & RequiredRflagsBit) ||
            (guestRflags & ReservedRflagsMask) ||
            (guestRflags & VMBit) ||
            readOrZero(vmcs, VmcsGuestActivityState) != 0 ||
            readOrZero(vmcs, VmcsField::GuestInterruptibilityState) != 0 ||
            readOrZero(vmcs, VmcsField::VmcsLinkPointer) != mask(64) ||
            (guestIa32e && (!guestCr0Bits.pg || !guestCr4Bits.pae))) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }

    if (entryControls & VmEntryLoadIa32Efer) {
        const uint64_t guestEferValue =
            readOrZero(vmcs, VmcsGuestIa32Efer);
        Efer guestEfer = guestEferValue;
        if ((guestEferValue & ~SupportedEferBits) ||
                guestEfer.lma != guestIa32e ||
                (guestCr0Bits.pg && guestEfer.lme != guestIa32e)) {
            return failEntry(VmxExitReason::VmEntryInvalidGuestState);
        }
    }

    // SDM Vol. 3C, 29.3.1.1 requires these guest-state fields to be
    // canonical on processors that support Intel 64 architecture.
    if (!canonicalAddress(readOrZero(vmcs, VmcsGuestIa32SysenterEsp)) ||
            !canonicalAddress(readOrZero(vmcs, VmcsGuestIa32SysenterEip))) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }

    uint64_t guestCsAccessRights = 0;
    uint64_t guestCsSelector = 0;
    if (!readRequired(vmcs, VmcsField::GuestCsAccessRights,
                guestCsAccessRights) ||
            !readRequired(vmcs, VmcsField::GuestCsSelector,
                guestCsSelector)) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }
    if (guestIa32e && bits(guestCsAccessRights, 13) &&
            !uniformUpperLinearBits(guestRip)) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }
    if ((!guestIa32e || !bits(guestCsAccessRights, 13)) &&
            bits(guestRip, 63, 32) != 0) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }

    for (const auto &segment : GuestSegments) {
        uint64_t selector = 0;
        uint64_t base = 0;
        uint64_t limit = 0;
        uint64_t attrValue = 0;
        if (!readRequired(vmcs, segment.selector, selector) ||
                !readRequired(vmcs, segment.base, base) ||
                !readRequired(vmcs, segment.limit, limit) ||
                !readRequired(vmcs, segment.attr, attrValue)) {
            return failEntry(VmxExitReason::VmEntryInvalidGuestState);
        }

        if (!validGuestSegment(segment.index, bits(selector, 15, 0), base,
                    bits(limit, 31, 0), bits(attrValue, 31, 0), guestIa32e,
                    bits(guestCsSelector, 1, 0))) {
            return failEntry(VmxExitReason::VmEntryInvalidGuestState);
        }
    }

    const uint64_t guestGdtrBase =
        readOrZero(vmcs, VmcsField::GuestGdtrBase);
    const uint64_t guestIdtrBase =
        readOrZero(vmcs, VmcsField::GuestIdtrBase);
    const uint64_t guestGdtrLimit =
        readOrZero(vmcs, VmcsField::GuestGdtrLimit);
    const uint64_t guestIdtrLimit =
        readOrZero(vmcs, VmcsField::GuestIdtrLimit);
    if ((guestIa32e && (!canonicalAddress(guestGdtrBase) ||
                       !canonicalAddress(guestIdtrBase))) ||
            (!guestIa32e && (bits(guestGdtrBase, 63, 32) != 0 ||
                             bits(guestIdtrBase, 63, 32) != 0)) ||
            bits(guestGdtrLimit, 31, 16) != 0 ||
            bits(guestIdtrLimit, 31, 16) != 0) {
        return failEntry(VmxExitReason::VmEntryInvalidGuestState);
    }

    return {};
}

bool
VmxState::loadGuestState(ThreadContext *tc, Vmcs &vmcs, Addr &guestRip) const
{
    const uint64_t entryControls = readOrZero(vmcs, VmcsVmEntryControls);
    const uint64_t guestCr0 = readOrZero(vmcs, VmcsGuestCr0);
    const uint64_t guestCr3 = readOrZero(vmcs, VmcsGuestCr3);
    const uint64_t guestCr4 = readOrZero(vmcs, VmcsGuestCr4);
    const uint64_t guestRsp = readOrZero(vmcs, VmcsGuestRsp);
    const uint64_t guestRflags = readOrZero(vmcs, VmcsGuestRflags);

    Efer guestEfer = tc->readMiscRegNoEffect(misc_reg::Efer);
    if (entryControls & VmEntryLoadIa32Efer) {
        guestEfer = readOrZero(vmcs, VmcsGuestIa32Efer);
    } else {
        const bool guestIa32e = entryControls & VmEntryIa32eModeGuest;
        guestEfer.lma = guestIa32e;
        CR0 guestCr0Bits = guestCr0;
        if (guestCr0Bits.pg) {
            guestEfer.lme = guestIa32e;
        }
    }
    tc->setMiscRegNoEffect(misc_reg::Efer, guestEfer);
    tc->setMiscReg(misc_reg::Cr4, guestCr4);
    tc->setMiscReg(misc_reg::Cr0, vmx::mergeLoadedCr0(
                tc->readMiscRegNoEffect(misc_reg::Cr0), guestCr0));
    tc->setMiscReg(misc_reg::Cr3, guestCr3);
    // Load-debug-controls is not advertised. Intel specifies the reset DR7
    // value when that entry control is clear.
    tc->setMiscReg(misc_reg::Dr7, 0x400);

    for (const auto &segment : GuestSegments) {
        tc->setMiscReg(misc_reg::segSel(segment.index),
                readOrZero(vmcs, segment.selector));
        tc->setMiscReg(misc_reg::segBase(segment.index),
                readOrZero(vmcs, segment.base));
        tc->setMiscReg(misc_reg::segLimit(segment.index),
                readOrZero(vmcs, segment.limit));
        tc->setMiscReg(misc_reg::segAttr(segment.index),
                vmcsAccessRightsToSegAttr(readOrZero(vmcs, segment.attr)));
    }

    tc->setMiscReg(misc_reg::TsgBase,
            readOrZero(vmcs, VmcsField::GuestGdtrBase));
    tc->setMiscReg(misc_reg::TsgLimit,
            readOrZero(vmcs, VmcsField::GuestGdtrLimit));
    tc->setMiscReg(misc_reg::IdtrBase,
            readOrZero(vmcs, VmcsField::GuestIdtrBase));
    tc->setMiscReg(misc_reg::IdtrLimit,
            readOrZero(vmcs, VmcsField::GuestIdtrLimit));

    tc->setMiscReg(misc_reg::SysenterCs,
            readOrZero(vmcs, VmcsGuestIa32SysenterCs));
    tc->setMiscReg(misc_reg::SysenterEsp,
            readOrZero(vmcs, VmcsGuestIa32SysenterEsp));
    tc->setMiscReg(misc_reg::SysenterEip,
            readOrZero(vmcs, VmcsGuestIa32SysenterEip));

    tc->setReg(int_reg::Rsp, guestRsp);
    setRFlags(tc, guestRflags);
    tc->setMiscReg(misc_reg::M5Reg, 0);

    guestRip = readOrZero(vmcs, VmcsGuestRip) +
        tc->readMiscRegNoEffect(misc_reg::CsEffBase);
    tc->getMMUPtr()->flushAll();
    DPRINTF(VMX, "VM-entry loaded guest state: RIP %#x RSP %#x "
            "CR0 %#x CR3 %#x CR4 %#x RFLAGS %#x\n",
            guestRip, guestRsp, guestCr0, guestCr3, guestCr4, guestRflags);
    return true;
}

void
VmxState::saveGuestState(ThreadContext *tc, Vmcs &vmcs) const
{
    vmcs.writeUnchecked(VmcsGuestCr0,
            tc->readMiscRegNoEffect(misc_reg::Cr0));
    vmcs.writeUnchecked(VmcsGuestCr3,
            tc->readMiscRegNoEffect(misc_reg::Cr3));
    vmcs.writeUnchecked(VmcsGuestCr4,
            tc->readMiscRegNoEffect(misc_reg::Cr4));
    if (readOrZero(vmcs, VmcsVmExitControls) & VmExitSaveDebugControls) {
        vmcs.writeUnchecked(VmcsGuestDr7,
                tc->readMiscRegNoEffect(misc_reg::Dr7));
    }
    vmcs.writeUnchecked(VmcsGuestRsp, tc->getReg(int_reg::Rsp));
    vmcs.writeUnchecked(VmcsGuestRip, currentRip(tc));
    vmcs.writeUnchecked(VmcsGuestRflags, getRFlags(tc));
    if (readOrZero(vmcs, VmcsVmExitControls) & VmExitSaveIa32Efer) {
        vmcs.writeUnchecked(VmcsGuestIa32Efer,
                tc->readMiscRegNoEffect(misc_reg::Efer));
    }

    for (const auto &segment : GuestSegments) {
        vmcs.writeUnchecked(segment.selector,
                tc->readMiscRegNoEffect(misc_reg::segSel(segment.index)));
        vmcs.writeUnchecked(segment.base,
                tc->readMiscRegNoEffect(misc_reg::segBase(segment.index)));
        vmcs.writeUnchecked(segment.limit,
                tc->readMiscRegNoEffect(misc_reg::segLimit(segment.index)));
        vmcs.writeUnchecked(segment.attr,
                segAttrToVmcsAccessRights(tc->readMiscRegNoEffect(
                    misc_reg::segAttr(segment.index))));
    }

    vmcs.writeUnchecked(VmcsField::GuestGdtrBase,
            tc->readMiscRegNoEffect(misc_reg::TsgBase));
    vmcs.writeUnchecked(VmcsField::GuestGdtrLimit,
            tc->readMiscRegNoEffect(misc_reg::TsgLimit));
    vmcs.writeUnchecked(VmcsField::GuestIdtrBase,
            tc->readMiscRegNoEffect(misc_reg::IdtrBase));
    vmcs.writeUnchecked(VmcsField::GuestIdtrLimit,
            tc->readMiscRegNoEffect(misc_reg::IdtrLimit));

    vmcs.writeUnchecked(VmcsGuestIa32SysenterCs,
            tc->readMiscRegNoEffect(misc_reg::SysenterCs));
    vmcs.writeUnchecked(VmcsGuestIa32SysenterEsp,
            tc->readMiscRegNoEffect(misc_reg::SysenterEsp));
    vmcs.writeUnchecked(VmcsGuestIa32SysenterEip,
            tc->readMiscRegNoEffect(misc_reg::SysenterEip));

    DPRINTF(VMX, "VM-exit saved guest state: RIP %#x RSP %#x RFLAGS %#x\n",
            readOrZero(vmcs, VmcsGuestRip), readOrZero(vmcs, VmcsGuestRsp),
            readOrZero(vmcs, VmcsGuestRflags));
}

bool
VmxState::loadHostState(ThreadContext *tc, Vmcs &vmcs, Addr &hostRip) const
{
    const uint64_t exitControls = readOrZero(vmcs, VmcsVmExitControls);
    const bool hostIa32e = exitControls & VmExitHostAddressSpaceSize;

    uint64_t hostCr0 = 0;
    uint64_t hostCr3 = 0;
    uint64_t hostCr4 = 0;
    uint64_t hostRsp = 0;
    if (!readRequired(vmcs, VmcsHostCr0, hostCr0) ||
            !readRequired(vmcs, VmcsHostCr3, hostCr3) ||
            !readRequired(vmcs, VmcsHostCr4, hostCr4) ||
            !readRequired(vmcs, VmcsHostRsp, hostRsp) ||
            !readRequired(vmcs, VmcsHostRip, hostRip)) {
        return false;
    }

    Efer hostEfer = tc->readMiscRegNoEffect(misc_reg::Efer);
    hostEfer.lma = hostIa32e;
    hostEfer.lme = hostIa32e;
    if (exitControls & VmExitLoadIa32Efer) {
        hostEfer = readOrZero(vmcs, VmcsHostIa32Efer);
    }
    tc->setMiscRegNoEffect(misc_reg::Efer, hostEfer);
    tc->setMiscReg(misc_reg::Cr4, hostCr4);
    tc->setMiscReg(misc_reg::Cr0, vmx::mergeLoadedCr0(
                tc->readMiscRegNoEffect(misc_reg::Cr0), hostCr0));
    tc->setMiscReg(misc_reg::Cr3, hostCr3);
    tc->setMiscReg(misc_reg::Dr7, 0x400);

    for (const auto &hostSelector : HostSelectors) {
        const uint64_t selector = readOrZero(vmcs, hostSelector.selector);
        uint64_t base = 0;
        uint64_t limit = mask(32);
        SegAttr attr = hostDataAttr();

        if (hostSelector.index == segment_idx::Cs) {
            attr = hostCodeAttr(hostIa32e);
        } else if (hostSelector.index == segment_idx::Tr) {
            base = readOrZero(vmcs, VmcsField::HostTrBase);
            limit = 0x67;
            attr = hostTssAttr();
        } else if (hostSelector.index == segment_idx::Fs) {
            base = readOrZero(vmcs, VmcsField::HostFsBase);
        } else if (hostSelector.index == segment_idx::Gs) {
            base = readOrZero(vmcs, VmcsField::HostGsBase);
        }

        if (selector == 0) {
            attr.unusable = 1;
        }
        tc->setMiscReg(misc_reg::segSel(hostSelector.index), selector);
        tc->setMiscReg(misc_reg::segBase(hostSelector.index), base);
        tc->setMiscReg(misc_reg::segLimit(hostSelector.index), limit);
        tc->setMiscReg(misc_reg::segAttr(hostSelector.index), attr);
    }

    SegAttr unusable = 0;
    unusable.unusable = 1;
    tc->setMiscReg(misc_reg::segSel(segment_idx::Tsl), 0);
    tc->setMiscReg(misc_reg::segBase(segment_idx::Tsl), 0);
    tc->setMiscReg(misc_reg::segLimit(segment_idx::Tsl), 0);
    tc->setMiscReg(misc_reg::segAttr(segment_idx::Tsl), unusable);

    tc->setMiscReg(misc_reg::TsgBase,
            readOrZero(vmcs, VmcsField::HostGdtrBase));
    tc->setMiscReg(misc_reg::TsgLimit, 0xffff);
    tc->setMiscReg(misc_reg::IdtrBase,
            readOrZero(vmcs, VmcsField::HostIdtrBase));
    tc->setMiscReg(misc_reg::IdtrLimit, 0xffff);

    tc->setMiscReg(misc_reg::SysenterCs,
            readOrZero(vmcs, VmcsHostIa32SysenterCs));
    tc->setMiscReg(misc_reg::SysenterEsp,
            readOrZero(vmcs, VmcsHostIa32SysenterEsp));
    tc->setMiscReg(misc_reg::SysenterEip,
            readOrZero(vmcs, VmcsHostIa32SysenterEip));

    tc->setReg(int_reg::Rsp, hostRsp);
    setRFlags(tc, RequiredRflagsBit);
    tc->setMiscReg(misc_reg::M5Reg, 0);
    tc->getMMUPtr()->flushAll();
    DPRINTF(VMX, "VM-exit loaded host state: RIP %#x RSP %#x "
            "CR0 %#x CR3 %#x CR4 %#x\n",
            hostRip, hostRsp, hostCr0, hostCr3, hostCr4);
    return true;
}

VmxResult
VmxState::failVmEntry(ThreadContext *tc, Vmcs &vmcs,
        VmxExitReason reason)
{
    const uint32_t encodedReason =
        toInt(reason) | VmExitReasonVmEntryFailure;
    vmcs.writeUnchecked(VmcsVmExitReason, encodedReason);
    vmcs.writeUnchecked(VmcsExitQualification, 0);
    // SDM 29.8 updates only the exit reason and qualification for an
    // invalid-guest-state VM-entry failure. Other VM-exit information fields
    // retain their previous values; clearing them here loses architecturally
    // visible diagnostic state.

    Addr hostRip = 0;
    panic_if(!loadHostState(tc, vmcs, hostRip),
            "Validated VM-entry host state could not be loaded");

    inVmxNonRoot = false;
    DPRINTF(VMX, "VM-entry failed after entry began: reason %u "
            "encoded %#x host RIP %#x VMCS %#x\n",
            toInt(reason), encodedReason, hostRip, currentVmcsPtr);
    return VmxResult::successRedirect(hostRip);
}

VmxResult
VmxState::vmEntry(ExecContext *xc, bool launch, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    Vmcs *vmcs = currentVmcs();

    DPRINTF(VMX, "%s start VMCS %#x\n",
            launch ? "VMLAUNCH" : "VMRESUME", currentVmcsPtr);

    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc,
                launch ? VmxExitReason::Vmlaunch : VmxExitReason::Vmresume,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }
    if (!vmcs) {
        return VmxResult::failInvalid();
    }
    if (launch && vmcs->launched()) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmlaunchNonClearVmcs));
    }
    if (!launch && !vmcs->launched()) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmresumeNonLaunchedVmcs));
    }

    const auto validation = validateVmEntry(tc, *vmcs);
    if (!validation.valid) {
        if (validation.vmEntryFailure) {
            DPRINTF(VMX, "VM-entry validation reached guest-state "
                    "failure class: reason %u\n",
                    toInt(validation.exitReason));
            // A late VM-entry failure is delivered through the host-state
            // path like a VM exit, but the VMLAUNCH operation sets launch
            // state only after guest-state and MSR loading succeed.
            return failVmEntry(tc, *vmcs, validation.exitReason);
        }

        DPRINTF(VMX, "VM-entry validation failed before entry: error %u\n",
                toInt(validation.instructionError));
        return vmFailValid(vmcs, toInt(validation.instructionError));
    }

    DPRINTF(VMX, "VM-entry validation passed: pin %#x primary %#x "
            "exit %#x entry %#x VMCS %#x\n",
            readOrZero(*vmcs, VmcsPinBasedVmExecControl),
            readOrZero(*vmcs, VmcsCpuBasedVmExecControl),
            readOrZero(*vmcs, VmcsVmExitControls),
            readOrZero(*vmcs, VmcsVmEntryControls), currentVmcsPtr);

    Addr guestRip = 0;
    if (!loadGuestState(tc, *vmcs, guestRip)) {
        return failVmEntry(tc, *vmcs,
                VmxExitReason::VmEntryInvalidGuestState);
    }

    inVmxNonRoot = true;
    if (launch) {
        vmcs->setLaunched(true);
    }

    DPRINTF(VMX, "%s entered VMX non-root at %#x using VMCS %#x\n",
            launch ? "VMLAUNCH" : "VMRESUME", guestRip, currentVmcsPtr);
    return VmxResult::successRedirect(guestRip);
}

bool
VmxState::vmexit(ThreadContext *tc, const VmxExitInfo &exitInfo,
        Addr *hostRip)
{
    Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint32_t reason = toInt(exitInfo.reason);
    if (exitInfo.vmEntryFailure) {
        reason |= VmExitReasonVmEntryFailure;
    }

    saveGuestState(tc, *vmcs);
    vmcs->writeUnchecked(VmcsVmExitReason, reason);
    vmcs->writeUnchecked(VmcsExitQualification,
            exitInfo.hasQualification ? exitInfo.qualification : 0);
    vmcs->writeUnchecked(VmcsVmExitInstructionLen,
            exitInfo.hasInstructionLength ? exitInfo.instructionLength : 0);
    vmcs->writeUnchecked(VmcsVmxInstructionInfo,
            exitInfo.hasInstructionInfo ? exitInfo.instructionInfo : 0);
    vmcs->writeUnchecked(VmcsVmExitInterruptionInfo,
            exitInfo.hasInterruptionInfo ? exitInfo.interruptionInfo : 0);
    vmcs->writeUnchecked(VmcsVmExitInterruptionErrorCode,
            exitInfo.hasInterruptionErrorCode ?
            exitInfo.interruptionErrorCode : 0);
    vmcs->writeUnchecked(VmcsGuestLinearAddress,
            exitInfo.hasGuestLinearAddress ?
            exitInfo.guestLinearAddress : 0);
    vmcs->writeUnchecked(VmcsGuestPhysicalAddress,
            exitInfo.hasGuestPhysicalAddress ?
            exitInfo.guestPhysicalAddress : 0);

    Addr hostRipValue = 0;
    if (!loadHostState(tc, *vmcs, hostRipValue)) {
        return false;
    }

    inVmxNonRoot = false;

    if (hostRip) {
        *hostRip = hostRipValue;
    }
    DPRINTF(VMX, "VM exit reason %u from VMCS %#x to host RIP %#x\n",
            reason, currentVmcsPtr, hostRipValue);
    return true;
}

bool
VmxState::vmexitEvent(ThreadContext *tc, const VmxExitInfo &exitInfo)
{
    Addr hostRip = 0;
    if (!vmexit(tc, exitInfo, &hostRip)) {
        return false;
    }

    redirectNow(tc, hostRip);
    return true;
}

VmxResult
VmxState::vmexitInstruction(ExecContext *xc, VmxExitReason reason,
        uint8_t instructionSize, uint64_t qualification,
        uint32_t instructionInfo)
{
    DPRINTF(VMX, "VM-exit requested by instruction reason %u size %u "
            "guest RIP %#x\n",
            toInt(reason), instructionSize, currentRip(xc->tcBase()));

    VmxExitInfo exitInfo;
    exitInfo.reason = reason;
    exitInfo.hasInstructionLength = true;
    exitInfo.instructionLength = instructionSize;
    exitInfo.hasQualification = qualification != 0;
    exitInfo.qualification = qualification;
    exitInfo.hasInstructionInfo = instructionInfo != 0;
    exitInfo.instructionInfo = instructionInfo;

    Addr hostRip = 0;
    panic_if(!vmexit(xc->tcBase(), exitInfo, &hostRip),
            "Instruction-triggered VM exit could not load host state");

    redirectNextPc(xc->tcBase(), hostRip);
    return VmxResult::successRedirect(hostRip);
}

VmxResult
VmxState::controlRegisterExit(ExecContext *xc, uint8_t cr,
        VmxCrAccessType type, uint8_t gpr, uint64_t value,
        uint8_t instructionSize, bool lmswMemoryOperand)
{
    return vmexitInstruction(xc, VmxExitReason::ControlRegisterAccess,
            instructionSize, crQualification(
                cr, type, gpr, value, lmswMemoryOperand));
}

VmxResult
VmxState::debugRegisterExit(ExecContext *xc, uint8_t dr, bool fromDr,
        uint8_t gpr, uint8_t instructionSize)
{
    return vmexitInstruction(xc, VmxExitReason::MovDr, instructionSize,
            drQualification(dr, fromDr, gpr));
}

bool
VmxState::shouldExitOnException(uint8_t vector, uint64_t errorCode) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t exceptionBitmap = 0;
    vmcs->read(VmcsExceptionBitmap, exceptionBitmap);
    const bool bitmapBit = bits(exceptionBitmap, vector);

    if (vector == 14) {
        uint64_t mask = 0;
        uint64_t match = 0;
        vmcs->read(VmcsPageFaultErrorCodeMask, mask);
        vmcs->read(VmcsPageFaultErrorCodeMatch, match);
        const bool matched = (errorCode & mask) == match;
        return bitmapBit ? matched : !matched;
    }

    return bitmapBit;
}

bool
VmxState::shouldExitOnExternalInterrupt() const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsPinBasedVmExecControl, controls);
    return controls & PinBasedExternalInterruptExiting;
}

bool
VmxState::shouldExitOnNmi() const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsPinBasedVmExecControl, controls);
    return controls & PinBasedNmiExiting;
}

bool
VmxState::hltCausesExit() const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);
    return controls & CpuBasedHltExiting;
}

bool
VmxState::invlpgCausesExit() const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);
    return controls & CpuBasedInvlpgExiting;
}

bool
VmxState::movDrCausesExit() const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);
    return controls & CpuBasedMovDrExiting;
}

bool
VmxState::ioInstructionCausesExit(ThreadContext *tc, uint16_t port,
        size_t size) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);
    if (controls & CpuBasedUnconditionalIoExiting) {
        return true;
    }
    if (!(controls & CpuBasedUseIoBitmaps)) {
        return false;
    }

    PortProxy proxy(tc, tc->getSystemPtr()->cacheLineSize());
    for (size_t offset = 0; offset < size; ++offset) {
        const uint32_t checkedPort = port + offset;
        const VmcsField bitmapField =
            checkedPort < 0x8000 ? VmcsIoBitmapA : VmcsIoBitmapB;
        uint64_t bitmapBase = 0;
        vmcs->read(bitmapField, bitmapBase);

        const uint32_t bit = checkedPort & 0x7fff;
        uint8_t byte = 0;
        proxy.readBlob(bitmapBase + bit / 8, &byte, sizeof(byte));
        if (byte & (1u << (bit % 8))) {
            return true;
        }
    }

    return false;
}

bool
VmxState::rdmsrCausesExit(ThreadContext *tc, uint32_t msr) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);
    if (!(controls & CpuBasedUseMsrBitmaps)) {
        return true;
    }

    uint32_t index = 0;
    bool high = false;
    if (!isLowOrHighMsr(msr, index, high)) {
        return true;
    }

    uint64_t bitmapBase = 0;
    vmcs->read(VmcsMsrBitmap, bitmapBase);
    const Addr offset = (high ? 1024 : 0) + index / 8;
    uint8_t byte = 0;
    PortProxy proxy(tc, tc->getSystemPtr()->cacheLineSize());
    proxy.readBlob(bitmapBase + offset, &byte, sizeof(byte));
    return byte & (1u << (index % 8));
}

bool
VmxState::wrmsrCausesExit(ThreadContext *tc, uint32_t msr) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);
    if (!(controls & CpuBasedUseMsrBitmaps)) {
        return true;
    }

    uint32_t index = 0;
    bool high = false;
    if (!isLowOrHighMsr(msr, index, high)) {
        return true;
    }

    uint64_t bitmapBase = 0;
    vmcs->read(VmcsMsrBitmap, bitmapBase);
    const Addr offset = 2048 + (high ? 1024 : 0) + index / 8;
    uint8_t byte = 0;
    PortProxy proxy(tc, tc->getSystemPtr()->cacheLineSize());
    proxy.readBlob(bitmapBase + offset, &byte, sizeof(byte));
    return byte & (1u << (index % 8));
}

bool
VmxState::controlRegisterAccessCausesExit(uint8_t cr, VmxCrAccessType type,
        uint64_t value) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t controls = 0;
    vmcs->read(VmcsCpuBasedVmExecControl, controls);

    if (type == VmxCrAccessType::MovFromCr) {
        if (cr == 3) {
            return controls & CpuBasedCr3StoreExiting;
        }
        if (cr == 8) {
            return controls & CpuBasedCr8StoreExiting;
        }
        return false;
    }

    if (cr == 3) {
        return controls & CpuBasedCr3LoadExiting;
    }
    if (cr == 8) {
        return controls & CpuBasedCr8LoadExiting;
    }
    if (cr == 0 || cr == 4) {
        uint64_t guestHostMask = 0;
        uint64_t readShadow = 0;
        vmcs->read(cr == 0 ? VmcsCr0GuestHostMask : VmcsCr4GuestHostMask,
                guestHostMask);
        vmcs->read(cr == 0 ? VmcsCr0ReadShadow : VmcsCr4ReadShadow,
                readShadow);
        if (type == VmxCrAccessType::Clts) {
            return vmx::cltsCausesExit(guestHostMask, readShadow);
        }
        if (type == VmxCrAccessType::Lmsw) {
            return vmx::lmswCausesExit(guestHostMask, readShadow, value);
        }
        return ((value ^ readShadow) & guestHostMask) != 0;
    }

    return false;
}

uint64_t
VmxState::controlRegisterReadValue(uint8_t cr, uint64_t liveValue) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs || (cr != 0 && cr != 4)) {
        return liveValue;
    }

    uint64_t guestHostMask = 0;
    uint64_t readShadow = 0;
    vmcs->read(cr == 0 ? VmcsCr0GuestHostMask : VmcsCr4GuestHostMask,
            guestHostMask);
    vmcs->read(cr == 0 ? VmcsCr0ReadShadow : VmcsCr4ReadShadow,
            readShadow);
    return (liveValue & ~guestHostMask) | (readShadow & guestHostMask);
}

uint64_t
VmxState::controlRegisterWriteValue(uint8_t cr, uint64_t requestedValue,
        uint64_t liveValue) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs || (cr != 0 && cr != 4)) {
        return requestedValue;
    }

    uint64_t guestHostMask = 0;
    vmcs->read(cr == 0 ? VmcsCr0GuestHostMask : VmcsCr4GuestHostMask,
            guestHostMask);
    // Masked bits belong to the host and are never changed by a non-exiting
    // guest write. Unmasked bits receive the guest's requested value.
    return (liveValue & guestHostMask) |
        (requestedValue & ~guestHostMask);
}

bool
VmxState::controlRegisterWriteAllowed(ThreadContext *tc, uint8_t cr,
        uint64_t value) const
{
    if (!vmxActive || (cr != 0 && cr != 4)) {
        return true;
    }

    const auto *isa = static_cast<const ISA *>(tc->getIsaPtr());
    const RegIndex fixed0Reg = cr == 0 ? misc_reg::VmxCr0Fixed0 :
        misc_reg::VmxCr4Fixed0;
    const RegIndex fixed1Reg = cr == 0 ? misc_reg::VmxCr0Fixed1 :
        misc_reg::VmxCr4Fixed1;
    return vmx::fixedBitsAllowed(value, isa->readMiscRegNoEffect(fixed0Reg),
            isa->readMiscRegNoEffect(fixed1Reg));
}

Fault
VmxState::ioExitFault(bool read, uint16_t port, size_t size) const
{
    VmxExitInfo exitInfo;
    exitInfo.reason = VmxExitReason::IoInstruction;
    exitInfo.hasQualification = true;
    exitInfo.qualification = ioQualification(read, port, size);
    exitInfo.hasInstructionLength = true;
    exitInfo.instructionLength = 0;
    return std::make_shared<VmxExitFault>(exitInfo);
}

Fault
VmxState::msrExitFault(bool read, uint32_t msr) const
{
    VmxExitInfo exitInfo;
    exitInfo.reason = read ? VmxExitReason::Rdmsr : VmxExitReason::Wrmsr;
    exitInfo.hasInstructionLength = true;
    exitInfo.instructionLength = 0;
    return std::make_shared<VmxExitFault>(exitInfo);
}

Vmcs *
VmxState::findVmcs(Addr regionPtr)
{
    auto it = vmcsRegions.find(regionPtr);
    return it == vmcsRegions.end() ? nullptr : &it->second;
}

const Vmcs *
VmxState::findVmcs(Addr regionPtr) const
{
    auto it = vmcsRegions.find(regionPtr);
    return it == vmcsRegions.end() ? nullptr : &it->second;
}

Vmcs *
VmxState::currentVmcs()
{
    return currentVmcsPtr != InvalidVmcsPointer ?
        findVmcs(currentVmcsPtr) : nullptr;
}

const Vmcs *
VmxState::currentVmcs() const
{
    return currentVmcsPtr != InvalidVmcsPointer ?
        findVmcs(currentVmcsPtr) : nullptr;
}

VmxResult
VmxState::vmxon(ExecContext *xc, Addr operandEA,
        Request::Flags operandFlags, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (!vmxCr4Enabled(tc) || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }

    if (vmxActive) {
        if (inVmxNonRoot) {
            return vmexitInstruction(xc, VmxExitReason::Vmxon,
                    instructionSize);
        }
        if (!atCpl0(tc)) {
            return VmxResult::propagateFault(
                    std::make_shared<GeneralProtection>(0));
        }
        return vmFailIfCurrent(currentVmcs(),
                VmxInstructionError::VmxonInRoot);
    }

    if (!atCpl0(tc) || !vmxAvailable(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }

    auto fault = readOperand(xc, operandEA, operandFlags, regionPtr,
            sizeof(regionPtr));
    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    if (!validateRegion(tc, regionPtr)) {
        return VmxResult::failInvalid();
    }

    vmxActive = true;
    inVmxNonRoot = false;
    vmxonRegion = regionPtr;
    currentVmcsPtr = InvalidVmcsPointer;
    DPRINTF(VMX, "VMXON region %#x\n", vmxonRegion);
    return VmxResult::success();
}

VmxResult
VmxState::vmxoff(ExecContext *xc, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmxoff,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }

    for (auto &entry : vmcsRegions) {
        entry.second.setActive(false);
    }
    vmxActive = false;
    inVmxNonRoot = false;
    vmxonRegion = 0;
    currentVmcsPtr = InvalidVmcsPointer;
    DPRINTF(VMX, "VMXOFF\n");
    return VmxResult::success();
}

VmxResult
VmxState::vmclear(ExecContext *xc, Addr operandEA,
        Request::Flags operandFlags, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmclear,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }

    auto fault = readOperand(xc, operandEA, operandFlags, regionPtr,
            sizeof(regionPtr));

    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    Vmcs *current = currentVmcs();
    if (!validAlignedPhysicalAddress(regionPtr, Vmcs::VmcsRegionSize)) {
        return vmFailIfCurrent(current,
                VmxInstructionError::VmclearInvalidPhysicalAddress);
    }
    if (regionPtr == vmxonRegion) {
        return vmFailIfCurrent(current,
                VmxInstructionError::VmclearWithVmxonPointer);
    }
    if (currentVmcsPtr == regionPtr) {
        currentVmcsPtr = InvalidVmcsPointer;
    }

    auto [it, inserted] = vmcsRegions.try_emplace(
            regionPtr, regionPtr, vmcsRevisionId(tc));
    if (!inserted) {
        it->second.clear();
    }

    DPRINTF(VMX, "VMCLEAR VMCS %#x\n", regionPtr);
    return VmxResult::success();
}

VmxResult
VmxState::vmptrld(ExecContext *xc, Addr operandEA,
        Request::Flags operandFlags, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmptrld,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }

    auto fault = readOperand(xc, operandEA, operandFlags, regionPtr,
            sizeof(regionPtr));

    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    Vmcs *current = currentVmcs();
    if (!validAlignedPhysicalAddress(regionPtr, Vmcs::VmcsRegionSize)) {
        return vmFailIfCurrent(current,
                VmxInstructionError::VmptrldInvalidPhysicalAddress);
    }
    if (regionPtr == vmxonRegion) {
        return vmFailIfCurrent(current,
                VmxInstructionError::VmptrldWithVmxonPointer);
    }
    if (!validateRegion(tc, regionPtr)) {
        return vmFailIfCurrent(current,
                VmxInstructionError::VmptrldIncorrectVmcsRevision);
    }

    auto it = vmcsRegions.try_emplace(
            regionPtr, regionPtr, vmcsRevisionId(tc)).first;
    it->second.setActive(true);
    currentVmcsPtr = regionPtr;
    DPRINTF(VMX, "VMPTRLD current VMCS %#x\n", currentVmcsPtr);
    return VmxResult::success();
}

VmxResult
VmxState::vmptrst(ExecContext *xc, Addr operandEA,
        Request::Flags operandFlags, uint8_t instructionSize)
{
    uint64_t regionPtr = currentVmcsPtr;
    const std::vector<bool> byteEnable(sizeof(regionPtr), true);

    auto *tc = xc->tcBase();
    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmptrst,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }

    auto fault = vmxMemoryOperandFault(
            tc, operandEA, sizeof(regionPtr), operandFlags);
    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }
    fault = xc->writeMem(
            reinterpret_cast<uint8_t *>(&regionPtr), sizeof(regionPtr),
            operandEA, operandFlags, nullptr, byteEnable);
    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    return VmxResult::success();
}

VmxResult
VmxState::vmlaunch(ExecContext *xc, uint8_t instructionSize)
{
    return vmEntry(xc, true, instructionSize);
}

VmxResult
VmxState::vmresume(ExecContext *xc, uint8_t instructionSize)
{
    return vmEntry(xc, false, instructionSize);
}

VmxResult
VmxState::vmcall(ExecContext *xc, uint8_t instructionSize)
{
    Vmcs *vmcs = currentVmcs();
    auto *tc = xc->tcBase();

    DPRINTF(VMX, "VMCALL executed at RIP %#x vmxActive=%d nonRoot=%d "
            "VMCS %#x\n",
            currentRip(xc->tcBase()), vmxActive, inVmxNonRoot,
            currentVmcsPtr);

    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmcall, instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }
    if (!vmcs) {
        return VmxResult::failInvalid();
    }
    return vmFailValid(vmcs, toInt(VmxInstructionError::VmcallInRoot));
}

VmxResult
VmxState::vmread(ExecContext *xc, Vmcs::RawEncoding rawEncoding,
        uint64_t &value, uint8_t instructionSize)
{
    Vmcs *vmcs = currentVmcs();
    auto *tc = xc->tcBase();
    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmread,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }
    if (!vmcs) {
        return VmxResult::failInvalid();
    }

    Vmcs::FieldEncoding encoding;
    if (!Vmcs::decodeEncoding(rawEncoding, encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }
    if (!Vmcs::fieldSupported(encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }
    if (!vmcs->read(encoding, value)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }

    return VmxResult::success();
}

VmxResult
VmxState::vmwritePrecheck(ExecContext *xc, uint8_t instructionSize)
{
    Vmcs *vmcs = currentVmcs();
    auto *tc = xc->tcBase();

    if (!vmxActive || !vmxInstructionRecognized(tc)) {
        return VmxResult::propagateFault(std::make_shared<InvalidOpcode>());
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmwrite,
                instructionSize);
    }
    if (!atCpl0(tc)) {
        return VmxResult::propagateFault(std::make_shared<GeneralProtection>(
                    0));
    }
    if (!vmcs) {
        return VmxResult::failInvalid();
    }
    return VmxResult::success();
}

VmxResult
VmxState::vmwrite(ExecContext *xc, Vmcs::RawEncoding rawEncoding,
        uint64_t value, uint8_t instructionSize)
{
    VmxResult precheck = vmwritePrecheck(xc, instructionSize);
    if (!precheck.succeeded() || precheck.redirectsNextPc) {
        return precheck;
    }

    Vmcs *vmcs = currentVmcs();
    panic_if(!vmcs, "Successful VMWRITE precheck requires a current VMCS");
    Vmcs::FieldEncoding encoding;
    if (!Vmcs::decodeEncoding(rawEncoding, encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }
    if (!Vmcs::fieldSupported(encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }
    if (!Vmcs::fieldWritable(encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmwriteReadOnlyVmcsComponent));
    }

    if (!vmcs->write(encoding, value)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmwriteReadOnlyVmcsComponent));
    }

    return VmxResult::success();
}

void
VmxState::serialize(CheckpointOut &cp) const
{
    size_t numVmcsRegions = vmcsRegions.size();
    SERIALIZE_SCALAR(vmxActive);
    SERIALIZE_SCALAR(inVmxNonRoot);
    SERIALIZE_SCALAR(vmxonRegion);
    SERIALIZE_SCALAR(currentVmcsPtr);
    SERIALIZE_SCALAR(numVmcsRegions);
    // Serialize each VMCS region with a unique section name.
    size_t index = 0;
    for (const auto &[regionPtr, vmcs] : vmcsRegions) {
        Serializable::ScopedCheckpointSection sec(
                cp, "vmcsRegion" + std::to_string(index++));
        vmcs.serialize(cp);
    }
}

void
VmxState::unserialize(CheckpointIn &cp)
{
    // If VMX is inactive, ignore any stale serialized VMX state.
    if (!UNSERIALIZE_OPT_SCALAR(vmxActive)) {
        vmxActive = false;
        inVmxNonRoot = false;
        vmxonRegion = 0;
        currentVmcsPtr = InvalidVmcsPointer;
        vmcsRegions.clear();
        return;
    }

    size_t numVmcsRegions = 0;
    if (!UNSERIALIZE_OPT_SCALAR(inVmxNonRoot)) {
        inVmxNonRoot = false;
    }
    UNSERIALIZE_SCALAR(vmxonRegion);
    UNSERIALIZE_SCALAR(currentVmcsPtr);
    UNSERIALIZE_SCALAR(numVmcsRegions);

    vmcsRegions.clear();
    for (size_t index = 0; index < numVmcsRegions; ++index) {
        Serializable::ScopedCheckpointSection sec(
                cp, "vmcsRegion" + std::to_string(index));
        Vmcs vmcs;
        vmcs.unserialize(cp);
        const auto [it, inserted] =
            vmcsRegions.emplace(vmcs.pointer(), std::move(vmcs));
        panic_if(!inserted,
                "Malformed VMX checkpoint: duplicate VMCS pointer %#x",
                it->first);
    }

    panic_if(inVmxNonRoot && !vmxActive,
            "Malformed VMX checkpoint: non-root state without VMX operation");
    panic_if(vmxActive &&
            !validAlignedPhysicalAddress(vmxonRegion, PageBytes),
            "Malformed VMX checkpoint: invalid VMXON pointer %#x",
            vmxonRegion);
    Vmcs *current = currentVmcs();
    panic_if(currentVmcsPtr != InvalidVmcsPointer &&
            (!vmxActive || !current || !current->active()),
            "Malformed VMX checkpoint: invalid current VMCS pointer %#x",
            currentVmcsPtr);
    panic_if(inVmxNonRoot && !current,
            "Malformed VMX checkpoint: non-root state without current VMCS");
}

} // namespace X86ISA
} // namespace gem5
