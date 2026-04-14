#ifndef __ARCH_X86_VMCS_HH__
#define __ARCH_X86_VMCS_HH__

#include "base/types.hh"

namespace gem5
{
namespace X86ISA
{
// Intel VMX docs specify Vmcs section can contain at most 4KB
class alignas(4096) Vmcs
{
  public: 
    static constexpr size_t VmcsRegionSize = 4096; // 4K-Byte aligned memory
                                                   
    enum class LaunchState : uint8_t {
      Clear,
      Launched
    };

    class VmcsHeader
    {
      public:
        uint32_t revisionId; // Bit 30:0 Revision Identifier - Processors that maintain VMCS data in different formats use different revision identifiers
                             // Bit 31 indicates whether the VMCS is a shadow VMCS (Section 27.10)
                             // revisionId is NEVER written by the processor
                             // software can discover the revisionId that a processor uses by reading the VMX capability MSR IA32_VMX_BASIC (Appendix A.1)
        uint32_t abortIndicator; // Any non-zero number means proccessor threw abort signal
    };
    
    // Compile-Time Error handling to ensure perfect header on all systems
    static_assert(sizeof(VmcsHeader) == 8, "VMCS header must be exactly 8 bytes");
    static_assert(alignof(VmcsHeader) <= 4, "Unexpected alignment");
    static_assert(offsetof(VmcsHeader, abortIndicator) == 4, "abortIndicator must be at byte 4");


};
} // X86ISA
} // gem5

#endif // __ARCH_X86_VMCS_HH__
