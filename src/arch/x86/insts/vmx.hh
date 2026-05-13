#ifndef __ARCH_X86_VMX_HH__
#define __ARCH_X86_VMX_HH__

#include <array>
#include <cstdint>
#include <map>

#include "arch/x86/regs/segment.hh"
#include "arch/x86/vmcs.hh"
#include "base/types.hh"
#include "sim/faults.hh"
#include "sim/serialize.hh"

namespace gem5
{
class ExecContext;
class ThreadContext;

namespace X86ISA
{

enum class VmxStatus : uint8_t
{
    Success,
    VmFailInvalid,
    VmFailValid
};

struct VmxResult
{
    Fault fault = NoFault;
    VmxStatus status = VmxStatus::Success;
    uint32_t instructionError = 0;
    bool redirectsNextPc = false;
    Addr nextPc = 0;

    bool
    succeeded() const
    {
        return fault == NoFault && status == VmxStatus::Success;
    }

    static VmxResult success()
    {
        return {};
    }

    static VmxResult successRedirect(Addr next_pc)
    {
        VmxResult result;
        result.redirectsNextPc = true;
        result.nextPc = next_pc;
        return result;
    }

    static VmxResult failInvalid()
    {
        VmxResult result;
        result.status = VmxStatus::VmFailInvalid;
        return result;
    }

    static VmxResult failValid(uint32_t error)
    {
        VmxResult result;
        result.status = VmxStatus::VmFailValid;
        result.instructionError = error;
        return result;
    }

    static VmxResult propagateFault(const Fault &fault)
    {
        VmxResult result;
        result.fault = fault;
        return result;
    }
};

class VmxState
{
  private:
    using VmcsMap = std::map<Addr, Vmcs>;
    static constexpr size_t NumSegmentRegs = segment_idx::NumIdxs;

    struct RootSnapshot
    {
        bool valid = false;
        std::array<RegVal, NumSegmentRegs> selector = {};
        std::array<RegVal, NumSegmentRegs> base = {};
        std::array<RegVal, NumSegmentRegs> effBase = {};
        std::array<RegVal, NumSegmentRegs> limit = {};
        std::array<RegVal, NumSegmentRegs> attr = {};
        RegVal m5Reg = 0;

        void capture(ThreadContext *tc);
        void restore(ThreadContext *tc) const;
        void clear();
        void serialize(CheckpointOut &cp) const;
        void unserialize(CheckpointIn &cp);
    };

    bool vmxActive = false;
    bool inVmxNonRoot = false;
    Addr vmxonRegion = 0;
    Addr currentVmcsPtr = 0;
    VmcsMap vmcsRegions;
    RootSnapshot rootSnapshot;

    Vmcs *findVmcs(Addr regionPtr);
    const Vmcs *findVmcs(Addr regionPtr) const;
    Vmcs *currentVmcs();
    const Vmcs *currentVmcs() const;

  public:
    bool active() const { return vmxActive; }
    bool nonRootActive() const { return inVmxNonRoot; }
    Addr vmxonPtr() const { return vmxonRegion; }
    Addr currentVmcsPointer() const { return currentVmcsPtr; }

    VmxResult vmxon(ExecContext *xc, Addr operandEA);
    VmxResult vmxoff();
    VmxResult vmclear(ExecContext *xc, Addr operandEA);
    VmxResult vmptrld(ExecContext *xc, Addr operandEA);
    VmxResult vmptrst(ExecContext *xc, Addr operandEA);
    VmxResult vmlaunch(ExecContext *xc);
    VmxResult vmresume(ExecContext *xc);
    VmxResult vmcall(ExecContext *xc, uint8_t instructionSize);

    VmxResult vmread(Vmcs::RawEncoding encoding, uint64_t &value) const;
    VmxResult vmwrite(Vmcs::RawEncoding encoding, uint64_t value);

    void serialize(CheckpointOut &cp) const;
    void unserialize(CheckpointIn &cp);
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_VMX_HH__
