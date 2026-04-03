#ifndef __ARCH_X86_VMCS_HH__
#define __ARCH_X86_VMCS_HH__

#include "base/types.hh"

namespace gem5
{
namespace X86ISA
{
class Vmcs
{
  public: 
    static constexpr size_t VmcsRegionSize = 4096; // 4K-Byte aligned memory
	
    enum class LaunchState {
      Clear,
      Launched
    };

    class VmcsHeader
    {
      public:
        uint32_t revisionId;
        uint32_t abortIndicator;
    }
  private:
    VmcsHeader header;

};
} // X86ISA
} // gem5

#endif // __ARCH_X86_VMCS_HH__
