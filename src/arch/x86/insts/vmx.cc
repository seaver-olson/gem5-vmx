#include "arch/x86/insts/vmx.hh"

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
// Reads 64-bit mem operand
bool
readQwordOperand(ExecContext *xc, Addr operandEA, uint64_t &value)
{
    const std::vector<bool> byteEnable;
    auto fault = xc->readMem(
            operandEA, reinterpret_cast<uint8_t *>(&value), sizeof(value),
            Request::Flags(0), byteEnable);
    return fault == NoFault;
}

// Reads the VMCS region header from memory into the provided header struct
bool
readVmcsHeader(ThreadContext *tc, Addr regionPtr, Vmcs::VmcsHeader &header)
{
    tc->getSystemPtr()->physProxy.readBlob(regionPtr, &header, sizeof(header));
    return true;
}

// Checks if VMX is available and enabled in the current environment
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

// Validates that the given region pointer is properly aligned, has a valid revision ID, and is not a shadow VMCS
bool
validateRegion(ThreadContext *tc, Addr regionPtr)
{
    if (regionPtr & (Vmcs::VmcsRegionSize - 1)) {
        return false;
    }

    Vmcs::VmcsHeader header;
    readVmcsHeader(tc, regionPtr, header);

    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    const auto vmxBasic = isa->readMiscRegNoEffect(misc_reg::VmxBasic);
    const uint32_t expectedRevision = bits(vmxBasic, 30, 0);

    return bits(header.revisionId, 30, 0) == expectedRevision &&
           !bits(header.revisionId, 31);
}

} // namespace (makes this private to this file)

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
        return false;
    }

    bool
    VmxState::vmwrite(uint64_t encoding, uint64_t value)
    {
        return false;
    }
}
}
