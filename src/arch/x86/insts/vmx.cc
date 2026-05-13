#include "arch/x86/insts/vmx.hh"

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
constexpr uint32_t VmxErrUnsupportedComponent = 12;
constexpr uint32_t VmxErrWriteReadOnlyComponent = 13;
constexpr uint32_t VmxErrVmcallInRoot = 1;
constexpr uint32_t VmxErrVmlaunchNonClearVmcs = 4;
constexpr uint32_t VmxErrVmresumeNonLaunchedVmcs = 5;
constexpr uint32_t VmxExitReasonVmcall = 18;
constexpr Vmcs::Encoding VmcsVmExitReason = 0x4402;
constexpr Vmcs::Encoding VmcsVmExitInstructionLen = 0x440C;
constexpr Vmcs::Encoding VmcsGuestRip = 0x681E;
constexpr Vmcs::Encoding VmcsHostRip = 0x6C16;

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

} // namespace

void
VmxState::RootSnapshot::capture(ThreadContext *tc)
{
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
VmxState::vmxon(ExecContext *xc, Addr operandEA)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;
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
VmxState::vmxoff()
{
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
VmxState::vmclear(ExecContext *xc, Addr operandEA)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;
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
VmxState::vmptrld(ExecContext *xc, Addr operandEA)
{
    auto *tc = xc->tcBase(); // vmptrld does not require VMX to be active, but the operand still needs to be validated
    uint64_t regionPtr = 0;
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
VmxState::vmptrst(ExecContext *xc, Addr operandEA)
{
    uint64_t regionPtr = currentVmcsPtr ? currentVmcsPtr : mask(64); // if no current VMCS, return all 1s per SDM
    const std::vector<bool> byteEnable(sizeof(regionPtr), true); // Enable all bytes for the write

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
VmxState::vmlaunch(ExecContext *xc)
{
    auto *tc = xc->tcBase();
    Vmcs *vmcs = currentVmcs();

    if (!vmxActive || inVmxNonRoot || !vmcs) {
        return VmxResult::failInvalid();
    }
    if (vmcs->launched()) {
        return vmFailValid(vmcs, VmxErrVmlaunchNonClearVmcs);
    }

    uint64_t guestRip = 0;
    if (!vmcs->read(VmcsGuestRip, guestRip)) {
        return vmFailValid(vmcs, VmxErrUnsupportedComponent);
    }

    rootSnapshot.capture(tc);
    inVmxNonRoot = true;
    vmcs->setLaunched(true);
    return VmxResult::successRedirect(guestRip);
}

VmxResult
VmxState::vmresume(ExecContext *xc)
{
    auto *tc = xc->tcBase();
    Vmcs *vmcs = currentVmcs();

    if (!vmxActive || inVmxNonRoot || !vmcs) {
        return VmxResult::failInvalid();
    }
    if (!vmcs->launched()) {
        return vmFailValid(vmcs, VmxErrVmresumeNonLaunchedVmcs);
    }

    uint64_t guestRip = 0;
    if (!vmcs->read(VmcsGuestRip, guestRip)) {
        return vmFailValid(vmcs, VmxErrUnsupportedComponent);
    }

    rootSnapshot.capture(tc);
    inVmxNonRoot = true;
    return VmxResult::successRedirect(guestRip);
}

VmxResult
VmxState::vmcall(ExecContext *xc, uint8_t instructionSize)
{
    auto *tc = xc->tcBase();
    Vmcs *vmcs = currentVmcs();

    if (!vmxActive || !vmcs) {
        return VmxResult::failInvalid();
    }
    if (!inVmxNonRoot) {
        return vmFailValid(vmcs, VmxErrVmcallInRoot);
    }

    uint64_t hostRip = 0;
    if (!vmcs->read(VmcsHostRip, hostRip)) {
        return vmFailValid(vmcs, VmxErrUnsupportedComponent);
    }

    vmcs->writeUnchecked(VmcsGuestRip, currentRip(tc));
    vmcs->writeUnchecked(VmcsVmExitReason, VmxExitReasonVmcall);
    vmcs->writeUnchecked(VmcsVmExitInstructionLen, instructionSize);

    rootSnapshot.restore(tc);
    inVmxNonRoot = false;
    return VmxResult::successRedirect(hostRip);
}

VmxResult
VmxState::vmread(Vmcs::RawEncoding rawEncoding, uint64_t &value) const
{
    const Vmcs *vmcs = currentVmcs();
    // If there is no current VMCS or VMX is not active, instruction fails with VMfailInvalid (make sure to select a vmcs with vmptrld before vmwrite or vmread)
    if (!vmxActive || !vmcs) {
        return VmxResult::failInvalid(); // CF=1, ZF=0, VM-instruction error field = 0
    }

    Vmcs::Encoding encoding = 0;
    // Decodes the raw VMCS-field encoding operand (64-bit immediate) to determine the VMCS field to be accessed, also rejects invalid encodings (i.e. reserved bits set)
    if (!Vmcs::decodeEncoding(rawEncoding, encoding)) {
        return vmFailValid(const_cast<Vmcs *>(vmcs),
                VmxErrUnsupportedComponent); // CF=1, ZF=0, VM-instruction error field = 12 (VMXErrUnsupportedComponent)
    }
    // looks up the VMCS field specified, if the field is not supported(not in supportedFields[]), the instruction fails with VMfailValid
    if (!Vmcs::fieldSupported(encoding)) {
        return vmFailValid(const_cast<Vmcs *>(vmcs),
                VmxErrUnsupportedComponent); // CF=1, ZF=0, VM-instruction error field = 12 (VMXErrUnsupportedComponent)
    }
    // checks if the field is readable. If not, the instruction fails with VMfailValid
    if (!vmcs->read(encoding, value)) {
        return vmFailValid(const_cast<Vmcs *>(vmcs),
                VmxErrUnsupportedComponent); // CF=1, ZF=0, VM-instruction error field = 12 (VMXErrUnsupportedComponent)
    }

    return VmxResult::success(); // Field read, CF=0, ZF=0
}

VmxResult
VmxState::vmwrite(Vmcs::RawEncoding rawEncoding, uint64_t value)
{
    Vmcs *vmcs = currentVmcs();

    // If there is no current VMCS or VMX is not active, instruction fails with VMfailInvalid (make sure to select a vmcs with vmptrld before vmwrite or vmread)
    if (!vmxActive || !vmcs) {
        return VmxResult::failInvalid(); // CF=1, ZF=0, VM-instruction error field = 0
    }
    // Decodes the raw VMCS-field encoding operand (64-bit immediate) to determine the VMCS field to be accessed, also rejects invalid encodings (i.e. reserved bits set)
    Vmcs::Encoding encoding = 0;
    if (!Vmcs::decodeEncoding(rawEncoding, encoding)) {
        return vmFailValid(vmcs, VmxErrUnsupportedComponent); // CF=1, ZF=0, VM-instruction error field = 12 (VMXErrUnsupportedComponent)
    }
    // looks up the VMCS field specified, if the field is not supported(not in supportedFields[]), the instruction fails with VMfailValid
    if (!Vmcs::fieldSupported(encoding)) {
        return vmFailValid(vmcs, VmxErrUnsupportedComponent); // CF=1, ZF=0, VM-instruction error field = 12 (VMXErrUnsupportedComponent)
    }
    // Once field is determined, checks if the field is read-only. If true then the instruction fails with VMfailValid
    if (!Vmcs::fieldWritable(encoding)) {
        return vmFailValid(vmcs, VmxErrWriteReadOnlyComponent); // CF=1, ZF=0, VM-instruction error field = 13 (VMXErrWriteReadOnlyComponent)
    }

    if (!vmcs->write(encoding, value)) {
        return vmFailValid(vmcs, VmxErrWriteReadOnlyComponent); // CF=1, ZF=0, VM-instruction error field = 13 (VMXErrWriteReadOnlyComponent)
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
