#include "arch/x86/insts/vmx.hh"

#include <string>
#include <utility>
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

uint32_t
vmcsRevisionId(ThreadContext *tc)
{
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    return bits(isa->readMiscRegNoEffect(misc_reg::VmxBasic), 30, 0);
}

bool
readQwordOperand(ExecContext *xc, Addr operandEA, uint64_t &value)
{
    const std::vector<bool> byteEnable;
    auto fault = xc->readMem(
            operandEA, reinterpret_cast<uint8_t *>(&value), sizeof(value),
            Request::Flags(0), byteEnable);
    return fault == NoFault;
}

bool
readVmcsHeader(ThreadContext *tc, Addr regionPtr, Vmcs::VmcsHeader &header)
{
    tc->getSystemPtr()->physProxy.readBlob(regionPtr, &header, sizeof(header));
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

bool
VmxState::vmxon(ExecContext *xc, Addr operandEA)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (vmxActive || !vmxAvailable(tc) || !readQwordOperand(xc, operandEA,
                regionPtr) || !validateRegion(tc, regionPtr)) {
        return false;
    }

    vmxActive = true;
    vmxonRegion = regionPtr;
    currentVmcsPtr = 0;
    return true;
}

bool
VmxState::vmxoff()
{
    if (!vmxActive) {
        return false;
    }

    vmxActive = false;
    vmxonRegion = 0;
    currentVmcsPtr = 0;
    return true;
}

bool
VmxState::vmclear(ExecContext *xc, Addr operandEA)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (!vmxActive || !readQwordOperand(xc, operandEA, regionPtr) ||
        regionPtr == vmxonRegion || !validateRegion(tc, regionPtr)) {
        return false;
    }

    if (currentVmcsPtr == regionPtr) {
        currentVmcsPtr = 0;
    }

    auto [it, inserted] = vmcsRegions.try_emplace(
            regionPtr, regionPtr, vmcsRevisionId(tc));
    if (!inserted) {
        it->second.clear();
    }

    return true;
}

bool
VmxState::vmptrld(ExecContext *xc, Addr operandEA)
{
    auto *tc = xc->tcBase();
    uint64_t regionPtr = 0;

    if (!vmxActive || !readQwordOperand(xc, operandEA, regionPtr) ||
        regionPtr == vmxonRegion || !validateRegion(tc, regionPtr)) {
        return false;
    }

    vmcsRegions.try_emplace(regionPtr, regionPtr, vmcsRevisionId(tc));
    currentVmcsPtr = regionPtr;
    return true;
}

bool
VmxState::vmptrst(ExecContext *xc, Addr operandEA)
{
    const std::vector<bool> byteEnable;
    uint64_t regionPtr = currentVmcsPtr ? currentVmcsPtr : mask(64);

    if (!vmxActive) {
        return false;
    }

    auto fault = xc->writeMem(
            reinterpret_cast<uint8_t *>(&regionPtr), sizeof(regionPtr),
            operandEA, Request::Flags(0), nullptr, byteEnable);
    return fault == NoFault;
}

bool
VmxState::vmread(uint64_t encoding, uint64_t &value) const
{
    const Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !vmcs) {
        return false;
    }

    return vmcs->read(encoding, value);
}

bool
VmxState::vmwrite(uint64_t encoding, uint64_t value)
{
    Vmcs *vmcs = currentVmcs();
    if (!vmxActive || !vmcs) {
        return false;
    }

    vmcs->write(encoding, value);
    return true;
}

void
VmxState::serialize(CheckpointOut &cp) const
{
    size_t numVmcsRegions = vmcsRegions.size();

    SERIALIZE_SCALAR(vmxActive);
    SERIALIZE_SCALAR(vmxonRegion);
    SERIALIZE_SCALAR(currentVmcsPtr);
    SERIALIZE_SCALAR(numVmcsRegions);

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
