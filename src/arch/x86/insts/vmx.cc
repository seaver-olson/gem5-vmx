#include "arch/x86/insts/vmx.hh"

#include <memory>
#include <string> // for std::to_string
#include <utility> // for std::pair and std::move
#include <vector>

#include "arch/x86/decoder.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/regs/misc.hh"
#include "base/bitfield.hh"
#include "cpu/exec_context.hh"
#include "cpu/thread_context.hh"
#include "mem/port_proxy.hh"
#include "sim/system.hh"

namespace gem5
{
namespace X86ISA
{
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

constexpr Vmcs::Encoding VmcsVmExitReason = 0x4402;
constexpr Vmcs::Encoding VmcsVmExitInterruptionInfo = 0x4404;
constexpr Vmcs::Encoding VmcsVmExitInterruptionErrorCode = 0x4406;
constexpr Vmcs::Encoding VmcsVmExitInstructionLen = 0x440C;
constexpr Vmcs::Encoding VmcsVmxInstructionInfo = 0x440E;
constexpr Vmcs::Encoding VmcsPinBasedVmExecControl = 0x4000;
constexpr Vmcs::Encoding VmcsCpuBasedVmExecControl = 0x4002;
constexpr Vmcs::Encoding VmcsExceptionBitmap = 0x4004;
constexpr Vmcs::Encoding VmcsPageFaultErrorCodeMask = 0x4006;
constexpr Vmcs::Encoding VmcsPageFaultErrorCodeMatch = 0x4008;
constexpr Vmcs::Encoding VmcsIoBitmapA = 0x2000;
constexpr Vmcs::Encoding VmcsIoBitmapB = 0x2002;
constexpr Vmcs::Encoding VmcsMsrBitmap = 0x2004;
constexpr Vmcs::Encoding VmcsGuestPhysicalAddress = 0x2400;
constexpr Vmcs::Encoding VmcsCr0GuestHostMask = 0x6000;
constexpr Vmcs::Encoding VmcsCr4GuestHostMask = 0x6002;
constexpr Vmcs::Encoding VmcsCr0ReadShadow = 0x6004;
constexpr Vmcs::Encoding VmcsCr4ReadShadow = 0x6006;
constexpr Vmcs::Encoding VmcsExitQualification = 0x6400;
constexpr Vmcs::Encoding VmcsGuestLinearAddress = 0x640A;
constexpr Vmcs::Encoding VmcsGuestRip = 0x681E;
constexpr Vmcs::Encoding VmcsHostRip = 0x6C16;

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

constexpr uint32_t VmExitReasonVmEntryFailure = 1u << 31;

uint32_t
vmcsRevisionId(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    return bits(isa->readMiscRegNoEffect(misc_reg::VmxBasic), 30, 0);
}

Fault
readOperand(ExecContext *xc, Addr operandEA, uint64_t &value, size_t size)
{
    const std::vector<bool> byteEnable(size, true);
    value = 0;
    auto fault = xc->readMem(
            operandEA, reinterpret_cast<uint8_t *>(&value), size,
            Request::Flags(0), byteEnable);
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
vmxAvailable(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    CR4 cr4 = isa->readMiscRegNoEffect(misc_reg::Cr4);
    const RegVal featureControl =
        isa->readMiscRegNoEffect(misc_reg::FeatureControl);

    const bool featureLocked = bits(featureControl, 0);
    const bool vmxonEnabled = bits(featureControl, 2);
    return cr4.vmxe && featureLocked && vmxonEnabled;
}

bool
validateRegion(ThreadContext *tc, Addr regionPtr)
{
    if (regionPtr & (Vmcs::VmcsRegionSize - 1)) {
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

Addr
currentRip(ThreadContext *tc)
{
    const auto &pc = tc->pcState().as<PCState>();
    const Addr csBase = tc->readMiscRegNoEffect(misc_reg::CsBase);
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

    if (!read) {
        qualification |= 1ull << 3;
    }
    qualification |= static_cast<uint64_t>(port) << 16;
    return qualification;
}

uint64_t
crQualification(uint8_t cr, VmxCrAccessType type, uint8_t gpr,
        uint64_t value)
{
    uint64_t qualification = cr & 0xf;
    qualification |= (static_cast<uint64_t>(type) & 0x3) << 4;
    qualification |= (static_cast<uint64_t>(gpr) & 0xf) << 8;
    if (type == VmxCrAccessType::Lmsw) {
        qualification |= (value & 0xffff) << 16;
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
    if (!isa || !isa->vmxState().vmexitEvent(tc, exitInfo)) {
        panic("Unable to complete VMX exit fault");
    }
}

bool
VmxState::vmexit(ThreadContext *tc, const VmxExitInfo &exitInfo,
        Addr *hostRip)
{
    Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !inVmxNonRoot || !vmcs) {
        return false;
    }

    uint64_t hostRipValue = 0;
    if (!vmcs->read(VmcsHostRip, hostRipValue)) {
        return false;
    }

    uint32_t reason = toInt(exitInfo.reason);
    if (exitInfo.vmEntryFailure) {
        reason |= VmExitReasonVmEntryFailure;
    }

    vmcs->writeUnchecked(VmcsGuestRip, currentRip(tc));
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

    rootSnapshot.restore(tc);
    inVmxNonRoot = false;

    if (hostRip) {
        *hostRip = hostRipValue;
    }
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
    VmxExitInfo exitInfo;
    exitInfo.reason = reason;
    exitInfo.hasInstructionLength = true;
    exitInfo.instructionLength = instructionSize;
    exitInfo.hasQualification = qualification != 0;
    exitInfo.qualification = qualification;
    exitInfo.hasInstructionInfo = instructionInfo != 0;
    exitInfo.instructionInfo = instructionInfo;

    Addr hostRip = 0;
    if (!vmexit(xc->tcBase(), exitInfo, &hostRip)) {
        return VmxResult::failInvalid();
    }

    redirectNextPc(xc->tcBase(), hostRip);
    return VmxResult::successRedirect(hostRip);
}

VmxResult
VmxState::controlRegisterExit(ExecContext *xc, uint8_t cr,
        VmxCrAccessType type, uint8_t gpr, uint64_t value,
        uint8_t instructionSize)
{
    return vmexitInstruction(xc, VmxExitReason::ControlRegisterAccess,
            instructionSize, crQualification(cr, type, gpr, value));
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
        const Vmcs::Encoding bitmapField =
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
        return ((value ^ readShadow) & guestHostMask) != 0;
    }

    return false;
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

void
VmxState::RootSnapshot::capture(ThreadContext *tc)
{
    panic_if(!tc, "RootSnapshot::capture requires a valid ThreadContext");
    for (size_t idx = 0; idx < NumSegmentRegs; ++idx) {
        selector[idx] = tc->readMiscRegNoEffect(misc_reg::segSel(idx));
        base[idx] = tc->readMiscRegNoEffect(misc_reg::segBase(idx));
        effBase[idx] = tc->readMiscRegNoEffect(misc_reg::segEffBase(idx));
        limit[idx] = tc->readMiscRegNoEffect(misc_reg::segLimit(idx));
        attr[idx] = tc->readMiscRegNoEffect(misc_reg::segAttr(idx));
    }
    m5Reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    valid = true;
}

void
VmxState::RootSnapshot::restore(ThreadContext *tc) const
{
    panic_if(!tc, "RootSnapshot::restore requires a valid ThreadContext");
    if (!valid) {
        return;
    }

    for (size_t idx = 0; idx < NumSegmentRegs; ++idx) {
        tc->setMiscRegNoEffect(misc_reg::segSel(idx), selector[idx]);
        tc->setMiscRegNoEffect(misc_reg::segBase(idx), base[idx]);
        tc->setMiscRegNoEffect(misc_reg::segEffBase(idx), effBase[idx]);
        tc->setMiscRegNoEffect(misc_reg::segLimit(idx), limit[idx]);
        tc->setMiscRegNoEffect(misc_reg::segAttr(idx), attr[idx]);
    }

    tc->setMiscRegNoEffect(misc_reg::M5Reg, m5Reg);
    tc->getDecoderPtr()->as<Decoder>().setM5Reg(m5Reg);
}

void
VmxState::RootSnapshot::clear()
{
    valid = false;
    selector = {};
    base = {};
    effBase = {};
    limit = {};
    attr = {};
    m5Reg = 0;
}

void
VmxState::RootSnapshot::serialize(CheckpointOut &cp) const
{
    SERIALIZE_SCALAR(valid);
    arrayParamOut(cp, "selector", selector.data(), NumSegmentRegs);
    arrayParamOut(cp, "base", base.data(), NumSegmentRegs);
    arrayParamOut(cp, "effBase", effBase.data(), NumSegmentRegs);
    arrayParamOut(cp, "limit", limit.data(), NumSegmentRegs);
    arrayParamOut(cp, "attr", attr.data(), NumSegmentRegs);
    SERIALIZE_SCALAR(m5Reg);
}

void
VmxState::RootSnapshot::unserialize(CheckpointIn &cp)
{
    if (!UNSERIALIZE_OPT_SCALAR(valid)) {
        clear();
        return;
    }

    arrayParamIn(cp, "selector", selector.data(), NumSegmentRegs);
    arrayParamIn(cp, "base", base.data(), NumSegmentRegs);
    arrayParamIn(cp, "effBase", effBase.data(), NumSegmentRegs);
    arrayParamIn(cp, "limit", limit.data(), NumSegmentRegs);
    arrayParamIn(cp, "attr", attr.data(), NumSegmentRegs);
    UNSERIALIZE_SCALAR(m5Reg);
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
    return currentVmcsPtr ? findVmcs(currentVmcsPtr) : nullptr;
}

const Vmcs *
VmxState::currentVmcs() const
{
    return currentVmcsPtr ? findVmcs(currentVmcsPtr) : nullptr;
}

VmxResult
VmxState::vmxon(ExecContext *xc, Addr operandEA, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmxon,
                instructionSize);
    }

    auto fault = readOperand(xc, operandEA, regionPtr, sizeof(regionPtr));

    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    if (vmxActive || !vmxAvailable(tc) || !validateRegion(tc, regionPtr)) {
        return VmxResult::failInvalid();
    }

    vmxActive = true;
    inVmxNonRoot = false;
    vmxonRegion = regionPtr;
    currentVmcsPtr = 0;
    rootSnapshot.clear();
    return VmxResult::success();
}

VmxResult
VmxState::vmxoff(ExecContext *xc, uint8_t instructionSize)
{
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmxoff,
                instructionSize);
    }

    if (!vmxActive) {
        return VmxResult::failInvalid();
    }

    vmxActive = false;
    inVmxNonRoot = false;
    vmxonRegion = 0;
    currentVmcsPtr = 0;
    rootSnapshot.clear();
    return VmxResult::success();
}

VmxResult
VmxState::vmclear(ExecContext *xc, Addr operandEA, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmclear,
                instructionSize);
    }

    auto fault = readOperand(xc, operandEA, regionPtr, sizeof(regionPtr));

    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    if (!vmxActive || regionPtr == vmxonRegion ||
        !validateRegion(tc, regionPtr)) {
        return VmxResult::failInvalid();
    }

    if (currentVmcsPtr == regionPtr) {
        currentVmcsPtr = 0;
    }

    auto [it, inserted] = vmcsRegions.try_emplace(
            regionPtr, regionPtr, vmcsRevisionId(tc));
    if (!inserted) {
        it->second.clear();
    }

    return VmxResult::success();
}

VmxResult
VmxState::vmptrld(ExecContext *xc, Addr operandEA, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();// VMPTRLD requires VMX operation and selects the current VMCS.
    uint64_t regionPtr = 0;

    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmptrld,
                instructionSize);
    }

    auto fault = readOperand(xc, operandEA, regionPtr, sizeof(regionPtr));

    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    if (!vmxActive || regionPtr == vmxonRegion ||
        !validateRegion(tc, regionPtr)) {
        return VmxResult::failInvalid();
    }

    vmcsRegions.try_emplace(regionPtr, regionPtr, vmcsRevisionId(tc));
    currentVmcsPtr = regionPtr;
    return VmxResult::success();
}

VmxResult
VmxState::vmptrst(ExecContext *xc, Addr operandEA, uint8_t instructionSize)
{
    uint64_t regionPtr = currentVmcsPtr ? currentVmcsPtr : mask(64); // if no current VMCS, return all 1s per SDM
    const std::vector<bool> byteEnable(sizeof(regionPtr), true); // Enable all bytes for the write

    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmptrst,
                instructionSize);
    }

    if (!vmxActive) {
        return VmxResult::failInvalid();
    }

    auto fault = xc->writeMem(
            reinterpret_cast<uint8_t *>(&regionPtr), sizeof(regionPtr),
            operandEA, Request::Flags(0), nullptr, byteEnable);
    if (fault != NoFault) {
        return VmxResult::propagateFault(fault);
    }

    return VmxResult::success();
}

VmxResult
VmxState::vmlaunch(ExecContext *xc, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    Vmcs *vmcs = currentVmcs();

    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmlaunch,
                instructionSize);
    }

    if (!vmxActive || inVmxNonRoot || !vmcs) {
        return VmxResult::failInvalid();
    }
    if (vmcs->launched()) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmlaunchNonClearVmcs));
    }

    uint64_t guestRip = 0;
    if (!vmcs->read(VmcsGuestRip, guestRip)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }

    rootSnapshot.capture(tc);
    inVmxNonRoot = true;
    vmcs->setLaunched(true);
    return VmxResult::successRedirect(guestRip);
}

VmxResult
VmxState::vmresume(ExecContext *xc, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    Vmcs *vmcs = currentVmcs();

    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmresume,
                instructionSize);
    }

    if (!vmxActive || inVmxNonRoot || !vmcs) {
        return VmxResult::failInvalid();
    }
    if (!vmcs->launched()) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmresumeNonLaunchedVmcs));
    }

    uint64_t guestRip = 0;
    if (!vmcs->read(VmcsGuestRip, guestRip)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent));
    }

    rootSnapshot.capture(tc);
    inVmxNonRoot = true;
    return VmxResult::successRedirect(guestRip);
}

VmxResult
VmxState::vmcall(ExecContext *xc, uint8_t instructionSize)
{
    Vmcs *vmcs = currentVmcs();

    if (!vmxActive || !vmcs) {
        return VmxResult::failInvalid();
    }
    if (!inVmxNonRoot) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmcallInRoot));
    }

    return vmexitInstruction(xc, VmxExitReason::Vmcall, instructionSize);
}

VmxResult
VmxState::vmread(ExecContext *xc, Vmcs::RawEncoding rawEncoding,
        uint64_t &value, uint8_t instructionSize)
{
    Vmcs *vmcs = currentVmcs();
    // If there is no current VMCS or VMX is not active, instruction fails with VMfailInvalid (make sure to select a vmcs with vmptrld before vmwrite or vmread)
    if (!vmxActive || !vmcs) {
        return VmxResult::failInvalid(); // CF=1, ZF=0, VM-instruction error field = 0
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmread,
                instructionSize);
    }

    Vmcs::Encoding encoding = 0;
    // Decodes the raw VMCS-field encoding operand (64-bit immediate) to determine the VMCS field to be accessed, also rejects invalid encodings (i.e. reserved bits set)
    if (!Vmcs::decodeEncoding(rawEncoding, encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 12
    }
    // looks up the VMCS field specified, if the field is not supported(not in supportedFields[]), the instruction fails with VMfailValid
    if (!Vmcs::fieldSupported(encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 12
    }
    // checks if the field is readable. If not, the instruction fails with VMfailValid
    if (!vmcs->read(encoding, value)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 12
    }

    return VmxResult::success(); // Field read, CF=0, ZF=0
}

VmxResult
VmxState::vmwrite(ExecContext *xc, Vmcs::RawEncoding rawEncoding,
        uint64_t value, uint8_t instructionSize)
{
    Vmcs *vmcs = currentVmcs();

    // If there is no current VMCS or VMX is not active, instruction fails with VMfailInvalid (make sure to select a vmcs with vmptrld before vmwrite or vmread)
    if (!vmxActive || !vmcs) {
        return VmxResult::failInvalid(); // CF=1, ZF=0, VM-instruction error field = 0
    }
    if (inVmxNonRoot) {
        return vmexitInstruction(xc, VmxExitReason::Vmwrite,
                instructionSize);
    }
    // Decodes the raw VMCS-field encoding operand (64-bit immediate) to determine the VMCS field to be accessed, also rejects invalid encodings (i.e. reserved bits set)
    Vmcs::Encoding encoding = 0;
    if (!Vmcs::decodeEncoding(rawEncoding, encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 12
    }
    // looks up the VMCS field specified, if the field is not supported(not in supportedFields[]), the instruction fails with VMfailValid
    if (!Vmcs::fieldSupported(encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::UnsupportedVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 12
    }
    // Once field is determined, checks if the field is read-only. If true then the instruction fails with VMfailValid
    if (!Vmcs::fieldWritable(encoding)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmwriteReadOnlyVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 13
    }

    if (!vmcs->write(encoding, value)) {
        return vmFailValid(vmcs,
                toInt(VmxInstructionError::VmwriteReadOnlyVmcsComponent)); // CF=1, ZF=0, VM-instruction error field = 13
    }

    return VmxResult::success(); // Field written, CF=0, ZF=0
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
    {
        Serializable::ScopedCheckpointSection sec(cp, "rootSnapshot");
        rootSnapshot.serialize(cp);
    }
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
    // if vmx is not active, skip serializing the rest of the state since it should be ignored on vmxoff and vmxon resets the state
    if (!UNSERIALIZE_OPT_SCALAR(vmxActive)) {
        vmxActive = false;
        inVmxNonRoot = false;
        vmxonRegion = 0;
        currentVmcsPtr = 0;
        vmcsRegions.clear();
        rootSnapshot.clear();
        return;
    }

    size_t numVmcsRegions = 0;
    if (!UNSERIALIZE_OPT_SCALAR(inVmxNonRoot)) {
        inVmxNonRoot = false;
    }
    UNSERIALIZE_SCALAR(vmxonRegion);
    UNSERIALIZE_SCALAR(currentVmcsPtr);
    UNSERIALIZE_SCALAR(numVmcsRegions);

    if (cp.sectionExists(Serializable::currentSection() + ".rootSnapshot")) {
        Serializable::ScopedCheckpointSection sec(cp, "rootSnapshot");
        rootSnapshot.unserialize(cp);
    } else {
        rootSnapshot.clear();
    }

    vmcsRegions.clear();
    for (size_t index = 0; index < numVmcsRegions; ++index) {
        Serializable::ScopedCheckpointSection sec(
                cp, "vmcsRegion" + std::to_string(index));
        Vmcs vmcs;
        vmcs.unserialize(cp);
        vmcsRegions.emplace(vmcs.pointer(), std::move(vmcs));
    }
}

} // namespace X86ISA
} // namespace gem5
