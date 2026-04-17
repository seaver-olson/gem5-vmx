#ifndef __ARCH_X86_VMX_HH__
#define __ARCH_X86_VMX_HH__

#include <cstdint>
#include <map>

#include "arch/x86/vmcs.hh"
#include "base/types.hh"
#include "sim/serialize.hh"

namespace gem5
{
class ExecContext;

namespace X86ISA
{

class VmxState
{
  private:
    using VmcsMap = std::map<Addr, Vmcs>;

    bool vmxActive = false;
    Addr vmxonRegion = 0;
    Addr currentVmcsPtr = 0;
    VmcsMap vmcsRegions;

    Vmcs *findVmcs(Addr regionPtr);
    const Vmcs *findVmcs(Addr regionPtr) const;
    Vmcs *currentVmcs();
    const Vmcs *currentVmcs() const;

  public:
    bool active() const { return vmxActive; }
    Addr vmxonPtr() const { return vmxonRegion; }
    Addr currentVmcsPointer() const { return currentVmcsPtr; }

    bool vmxon(ExecContext *xc, Addr operandEA);
    bool vmxoff();
    bool vmclear(ExecContext *xc, Addr operandEA);
    bool vmptrld(ExecContext *xc, Addr operandEA);
    bool vmptrst(ExecContext *xc, Addr operandEA);

    bool vmread(uint64_t encoding, uint64_t &value) const;
    bool vmwrite(uint64_t encoding, uint64_t value);

    void serialize(CheckpointOut &cp) const;
    void unserialize(CheckpointIn &cp);
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_VMX_HH__
