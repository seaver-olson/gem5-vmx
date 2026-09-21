/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __ARCH_X86_TRANSLATION_HH__
#define __ARCH_X86_TRANSLATION_HH__

#include <array>
#include <memory>

#include "arch/generic/mmu.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/regs/misc.hh"

namespace gem5
{
namespace X86ISA
{
class PagingPort;

// Captured by the MMU before handing a request to asynchronous work. Every
// consumer retains a const view; restart and finalization use this snapshot,
// never live control registers or a subsequently modified CPU request.
// EPT configuration/generations will extend this contract when implemented.
struct TranslationContext
{
    TlbContext tag;
    uint64_t generation;
    bool nonRoot;
    CR0 cr0;
    CR3 cr3;
    CR4 cr4;
    Efer efer;
    HandyM5Reg m5reg;
    std::array<RegVal, 4> paePdpte;
    unsigned physicalBits;
    LocalApicBase apicBase;
    RegVal pat;
    Addr linearAddress;
    Addr faultAddress;
    Request::Flags flags;
    BaseMMU::Mode accessMode;
    BaseMMU::Mode originalMode;
    enum class Stream { Instruction, Data } stream;
    PagingPort *port;
};
using TranslationContextPtr = std::shared_ptr<const TranslationContext>;

} // namespace X86ISA
} // namespace gem5
#endif
