#ifndef __ARCH_X86_VMX_HH__
#define __ARCH_X86_VMX_HH__

#include <array>
#include <cstdint>
#include <optional>

#include "base/types.hh"
#include "arch/x86/vmcs.hh"

namespace gem5
{
class ExecContext;

namespace X86ISA
{

class VmxState
{
  private:
    bool vmxActive = false;
    Addr vmxonRegion = 0;
    Addr currentVmcsPtr = 0;

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
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_VMX_HH__
