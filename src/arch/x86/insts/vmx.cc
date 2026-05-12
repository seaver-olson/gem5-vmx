#include "arch/x86/insts/vmx.hh"

#include <string> // for std::to_string
#include <utility> // for std::pair and std::move
#include <vector>

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

} // namespace

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
    vmxonRegion = regionPtr;
    currentVmcsPtr = 0;
    return VmxResult::success();
}

VmxResult
VmxState::vmxoff()
{
    if (!vmxActive) {
        return VmxResult::failInvalid();
    }

    vmxActive = false;
    vmxonRegion = 0;
    currentVmcsPtr = 0;
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
    // if vmx is not active, skip serializing the rest of the state since it should be ignored on vmxoff and vmxon resets the state
    if (!UNSERIALIZE_OPT_SCALAR(vmxActive)) {
        vmxActive = false;
        vmxonRegion = 0;
        currentVmcsPtr = 0;
        vmcsRegions.clear();
        return;
    }

    size_t numVmcsRegions = 0;
    UNSERIALIZE_SCALAR(vmxonRegion);
    UNSERIALIZE_SCALAR(currentVmcsPtr);
    UNSERIALIZE_SCALAR(numVmcsRegions);

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
