/*
 * Copyright (c) 2026 Seaver Olson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "test_objects/x86_interrupt_tester.hh"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "arch/x86/decoder.hh"
#include "arch/x86/generated/decoder.hh"
#include "arch/x86/regs/float.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/utility.hh"
#include "cpu/base.hh"
#include "sim/sim_exit.hh"

namespace gem5::X86ISA
{

void
InterruptTester::run(const std::string &scenario)
{
    if (scenario == "suspend") {
        tc->suspend();
        return;
    }
    if (scenario == "seed-thread-reset") {
        seedThreadReset();
        return;
    }
    if (scenario == "thread-reset") {
        checkThreadReset();
        return;
    }
    if (scenario == "check-thread-seed") {
        panic_if(tc->getReg(int_reg::Rax) != 0x12340000 ||
                     tc->readMiscRegNoEffect(misc_reg::Cr0) != 0x80000011 ||
                     tc->getReg(flatFloatRegClass[0]) != 0x56780000,
                 "Resetting another context modified this thread");
        return;
    }
    if (scenario == "check-reset-tsc") {
        const auto elapsed = tc->getCpuPtr()->curCycle() - resetCycle;
        panic_if(!elapsed || tc->readMiscReg(misc_reg::Tsc) != elapsed,
                 "TSC did not advance from the reset instant");
        return;
    }
    // Keep timer/message delivery from waking a CPU into unmapped code.
    tc->halt();
    if (scenario == "reset") {
        reset();
        panic("Unsupported APIC RESET returned");
    }
    if (scenario == "init") {
        panic_if(tc->readMiscRegNoEffect(misc_reg::Mxcsr) != 0x1f80 ||
                     tc->readMiscRegNoEffect(misc_reg::Ftw) != 0xffff,
                 "Fresh ISA state lost its floating-point defaults");
        const std::array<RegIndex, 5> indices = {
            misc_reg::Mxcsr, misc_reg::Ftw, misc_reg::Fcw, misc_reg::Fsw,
            misc_reg::X87Top};
        const std::array<RegVal, 5> values = {0x3f80, 0xaaaa, 0x077f, 0x0240,
                                              3};
        for (unsigned i = 0; i < indices.size(); ++i) {
            tc->setMiscRegNoEffect(indices[i], values[i]);
        }
        for (unsigned i = 0; i < float_reg::MicrofpBase; ++i) {
            tc->setReg(floatRegClass[i], RegVal(0x12340000 + i));
        }
        tc->setMiscReg(misc_reg::Cr0, 0x20000011);
        tc->setReg(int_reg::Rax, RegVal(0x1234));
        InitInterrupt(0).invoke(tc);
        for (unsigned i = 0; i < indices.size(); ++i) {
            panic_if(tc->readMiscRegNoEffect(indices[i]) != values[i],
                     "INIT changed FP control state at index %u", i);
        }
        for (unsigned i = 0; i < float_reg::MicrofpBase; ++i) {
            panic_if(tc->getReg(floatRegClass[i]) != 0x12340000 + i,
                     "INIT changed floating-point register %u", i);
        }
        const auto &pc = tc->pcState().as<PCState>();
        const auto halt_pc =
            romMicroPC(X86ISAInst::rom_labels::extern_label_initIntHalt);
        panic_if(tc->getReg(int_reg::Rax) ||
                     tc->readMiscRegNoEffect(misc_reg::Cr0) != 0x20000010 ||
                     tc->readMiscRegNoEffect(misc_reg::Cr3) ||
                     tc->readMiscRegNoEffect(misc_reg::Cr4) ||
                     tc->readMiscRegNoEffect(misc_reg::Cs) != 0xf000 ||
                     pc.instAddr() != 0xfffffff0 || pc.microPC() != halt_pc,
                 "INIT lost its core initialization or wait-for-SIPI state");
        return;
    }
    panic_if(scenario != "clear", "Unknown interrupt test: %s", scenario);

    seedInterrupts();
    const Tick expiration = apicTimerEvent.when();
    std::array<uint32_t, NUM_APIC_REGS> expected;
    std::copy(std::begin(regs), std::end(regs), expected.begin());
    expected[APIC_INTERRUPT_REQUEST(2)] = 0;
    expected[APIC_INTERRUPT_REQUEST(3)] = 0;
    expected[APIC_TRIGGER_MODE(3)] = 0;

    clearAll();
    panic_if(!std::equal(expected.begin(), expected.end(), std::begin(regs)) ||
                 ISRV != 64 || IRRV || !startedUp || pendingIPIs != 1 ||
                 !apicTimerEvent.scheduled() ||
                 apicTimerEvent.when() != expiration,
             "Interrupt clearing reset configuration or in-service state");
    panic_if(pendingSmi || pendingNmi || pendingExtInt || pendingInit ||
                 pendingStartup || pendingUnmaskableInt || smiVector ||
                 nmiVector || extIntVector || initVector || startupVector ||
                 checkInterruptsRaw(),
             "Interrupt clearing left a pending request");

    // The generic fallback must still dispatch clearAll() for controllers
    // that do not implement a distinct reset operation.
    pendingNmi = pendingUnmaskableInt = true;
    BaseInterrupts::reset();
    panic_if(
        pendingNmi || pendingUnmaskableInt ||
            !std::equal(expected.begin(), expected.end(), std::begin(regs)),
        "Generic reset no longer delegates to interrupt clearing");
    pendingIPIs = 0;
    regs[APIC_INTERRUPT_COMMAND_LOW] = 0;
    exitSimulationLoopClassic("interrupt timer observation", 0,
                              expiration + 1);
}

void
InterruptTester::seedInterrupts()
{
    // Include both a pending-only level interrupt and one in service.
    regs[APIC_IN_SERVICE(2)] = 1;
    ISRV = 64;
    regs[APIC_INTERRUPT_REQUEST(2)] = 1;
    regs[APIC_TRIGGER_MODE(2)] = 1;
    regs[APIC_INTERRUPT_REQUEST(3)] = 2;
    regs[APIC_TRIGGER_MODE(3)] = 2;
    IRRV = 97;
    pendingSmi = pendingNmi = pendingExtInt = true;
    pendingInit = pendingStartup = pendingUnmaskableInt = true;
    smiVector = nmiVector = extIntVector = initVector = startupVector = 7;
    startedUp = true;
    // Model an outstanding outgoing IPI. Clearing incoming requests must
    // not erase the accounting used by its later response callback.
    pendingIPIs = 1;
    regs[APIC_INTERRUPT_COMMAND_LOW] = 1 << 12;
    regs[APIC_LOGICAL_DESTINATION] = 0x12000000;
    regs[APIC_TASK_PRIORITY] = 0x20;
    setReg(APIC_LVT_TIMER, 0xd0);
    setReg(APIC_INITIAL_COUNT, 4);
}

void
InterruptTester::seedThreadReset()
{
    panic_if(tc->status() != ThreadContext::Suspended,
             "Thread reset fixture requires a suspended context");
    for (RegIndex i = 0; i < misc_reg::NumRegs; ++i) {
        if (misc_reg::isValid(i)) {
            tc->setMiscRegNoEffect(i, 0x5a5a5a5a);
        }
    }
    for (RegIndex i = 0; i < int_reg::NumRegs; ++i) {
        tc->setReg(flatIntRegClass[i], RegVal(0x12340000 + i));
    }
    for (RegIndex i = 0; i < float_reg::NumRegs; ++i) {
        tc->setReg(flatFloatRegClass[i], RegVal(0x56780000 + i));
    }
    setRFlags(tc, 0xed7);
    tc->setReg(cc_reg::Ecf, RegVal(1));
    tc->setReg(cc_reg::Ezf, RegVal(1));

    // Install a coherent long-mode decode state without walking page tables.
    tc->setMiscRegNoEffect(misc_reg::Cr0, 0x80000011);
    tc->setMiscRegNoEffect(misc_reg::Cr4, 0x20);
    tc->setMiscRegNoEffect(misc_reg::Efer, 0x500);
    SegAttr code_attr = 0;
    code_attr.longMode = 1;
    code_attr.present = 1;
    code_attr.system = 1;
    code_attr.type = 11;
    code_attr.readable = 1;
    tc->setMiscRegNoEffect(misc_reg::CsAttr, code_attr);
    tc->setMiscRegNoEffect(misc_reg::SsAttr, 0);
    tc->setMiscReg(misc_reg::M5Reg, 0);
    PCState pc(0x12345000);
    pc.size(7);
    pc.upc(romMicroPC(X86ISAInst::rom_labels::extern_label_initIntHalt));
    pc.nupc(pc.upc() + 1);
    tc->pcState(pc);
    checkDecode(5);
}

void
InterruptTester::checkDecode(unsigned size)
{
    // MOV AX, imm16 in real mode, MOV EAX, imm32 in long mode.
    const std::array<uint8_t, 8> bytes = {0xb8, 0x34, 0x12, 0x90,
                                          0x90, 0x90, 0x90, 0x90};
    auto *decoder = tc->getDecoderPtr();
    // Buffer reset remains the caller's responsibility, as in setReset().
    decoder->reset();
    panic_if(decoder->moreBytesSize() != bytes.size(),
             "Unexpected x86 decoder fetch width");
    std::memcpy(decoder->moreBytesPtr(), bytes.data(), bytes.size());
    PCState pc(tc->pcState().instAddr());
    decoder->moreBytes(pc, pc.instAddr());
    panic_if(!decoder->instReady() || !decoder->decode(pc) ||
                 pc.size() != size,
             "Decoder did not observe the expected operand size");
    decoder->reset();
}

void
InterruptTester::checkThreadReset()
{
    panic_if(!tc->getCpuPtr()->curCycle(),
             "Thread reset must be tested at a nonzero cycle");
    seedInterrupts();
    std::array<uint32_t, NUM_APIC_REGS> expected_apic;
    std::copy(std::begin(regs), std::end(regs), expected_apic.begin());
    const Tick expiration = apicTimerEvent.when();
    const auto status = tc->status();

    // Repeat without reseeding to check that reset is idempotent.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        tc->getIsaPtr()->resetThread();
        resetCycle = tc->getCpuPtr()->curCycle();
        for (RegIndex i = 0; i < int_reg::NumRegs; ++i) {
            const RegVal expected =
                i == int_reg::_RdxIdx ? 0x306a9 + tc->contextId() : 0;
            panic_if(tc->getReg(flatIntRegClass[i]) != expected,
                     "RESET integer register %u mismatch", i);
        }
        for (RegIndex i = 0; i < float_reg::NumRegs; ++i) {
            panic_if(tc->getReg(flatFloatRegClass[i]),
                     "RESET left floating-point register %u dirty", i);
        }
        for (RegIndex i = 0; i < cc_reg::NumRegs; ++i) {
            panic_if(tc->getReg(ccRegClass[i]),
                     "RESET left condition-code register %u dirty", i);
        }
        const std::pair<RegIndex, RegVal> expected_misc[] = {
            {misc_reg::Cr0, 0x60000010},
            {misc_reg::Cr2, 0},
            {misc_reg::Cr3, 0},
            {misc_reg::Cr4, 0},
            {misc_reg::Cr8, 0},
            {misc_reg::Efer, 0},
            {misc_reg::Xcr0, 1},
            {misc_reg::Dr0, 0},
            {misc_reg::Dr1, 0},
            {misc_reg::Dr2, 0},
            {misc_reg::Dr3, 0},
            {misc_reg::Dr6, 0xffff0ff0},
            {misc_reg::Dr7, 0x400},
            {misc_reg::Fcw, 0x40},
            {misc_reg::Ftw, 0x5555},
            {misc_reg::Mxcsr, 0x1f80},
            {misc_reg::Fsw, 0},
            {misc_reg::X87Top, 0},
            {misc_reg::Ftag, 0},
            {misc_reg::Fiseg, 0},
            {misc_reg::Fioff, 0},
            {misc_reg::Foseg, 0},
            {misc_reg::Fooff, 0},
            {misc_reg::Fop, 0},
            {misc_reg::DefType, 0},
            {misc_reg::Pat, 0x0007040600070406ULL},
            {misc_reg::Mtrrcap, 0x508},
            {misc_reg::McgCap, 0x104},
            {misc_reg::SysenterCs, 0},
            {misc_reg::SysenterEsp, 0},
            {misc_reg::SysenterEip, 0},
            {misc_reg::Star, 0},
            {misc_reg::Lstar, 0},
            {misc_reg::TscAux, 0}};
        for (const auto &[index, value] : expected_misc) {
            panic_if(tc->readMiscRegNoEffect(index) != value,
                     "RESET miscellaneous register %u mismatch", index);
        }
        for (RegIndex i = misc_reg::MtrrPhysBaseBase; i < misc_reg::Pat; ++i) {
            panic_if(tc->readMiscRegNoEffect(i),
                     "RESET left MTRR register %u dirty", i);
        }
        for (int seg = 0; seg < segment_idx::NumIdxs; ++seg) {
            const RegVal base = seg == segment_idx::Cs ? 0xffff0000 : 0;
            const RegVal selector = seg == segment_idx::Cs ? 0xf000 : 0;
            panic_if(tc->readMiscRegNoEffect(misc_reg::segBase(seg)) != base ||
                         tc->readMiscRegNoEffect(misc_reg::segEffBase(seg)) !=
                             base ||
                         tc->readMiscRegNoEffect(misc_reg::segSel(seg)) !=
                             selector ||
                         tc->readMiscRegNoEffect(misc_reg::segLimit(seg)) !=
                             0xffff,
                     "RESET segment %d mismatch", seg);
            if (seg <= segment_idx::Gs) {
                const SegAttr attr =
                    tc->readMiscRegNoEffect(misc_reg::segAttr(seg));
                const bool code = seg == segment_idx::Cs;
                panic_if(attr.dpl || attr.unusable || attr.defaultSize ||
                             attr.longMode || attr.avl || attr.granularity ||
                             !attr.present || attr.type != (code ? 11 : 3) ||
                             attr.writable != !code || !attr.readable ||
                             attr.expandDown || !attr.system,
                         "RESET segment %d attributes mismatch", seg);
            }
        }
        const SegAttr ldt = tc->readMiscRegNoEffect(misc_reg::TslAttr);
        const SegAttr tr = tc->readMiscRegNoEffect(misc_reg::TrAttr);
        panic_if(!ldt.unusable || !ldt.present || ldt.type != 2 ||
                     ldt.system || tr.unusable || !tr.present ||
                     tr.type != 3 || tr.system,
                 "RESET system-segment attributes mismatch");
        const HandyM5Reg mode = tc->readMiscRegNoEffect(misc_reg::M5Reg);
        const auto &pc = tc->pcState().as<PCState>();
        const LocalApicBase apic_base = tc->readMiscReg(misc_reg::ApicBase);
        panic_if(getRFlags(tc) != 2 || tc->readMiscReg(misc_reg::Tsc) ||
                     mode.mode != LegacyMode || mode.submode != RealMode ||
                     mode.cpl || mode.paging || mode.prot || mode.defOp != 1 ||
                     mode.defAddr != 1 || mode.stack != 1 ||
                     pc.instAddr() != 0xfffffff0 || pc.upc() ||
                     pc.nupc() != 1 || pc.size() || pc.npc() != 0xfffffff8 ||
                     tc->status() != status || apic_base.base != 0xfee00 ||
                     !apic_base.enable ||
                     apic_base.bsp != (tc->contextId() == 0),
                 "RESET flags, execution state, TSC, or APIC base mismatch");
        checkDecode(3);
        panic_if(!std::equal(expected_apic.begin(), expected_apic.end(),
                             std::begin(regs)) ||
                     ISRV != 64 || IRRV != 97 || !startedUp ||
                     pendingIPIs != 1 || !pendingSmi || !pendingNmi ||
                     !pendingExtInt || !pendingInit || !pendingStartup ||
                     !pendingUnmaskableInt || smiVector != 7 ||
                     nmiVector != 7 || extIntVector != 7 || initVector != 7 ||
                     startupVector != 7 || !apicTimerEvent.scheduled() ||
                     apicTimerEvent.when() != expiration,
                 "Thread reset modified the interrupt controller");
    }
    // Remove synthetic requests before advancing time to check TSC.
    deschedule(apicTimerEvent);
    pendingIPIs = 0;
    regs[APIC_INTERRUPT_COMMAND_LOW] = 0;
    clearAll();
}

void
InterruptTester::checkTimer()
{
    panic_if(IRRV != 0xd0 ||
                 !getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 0xd0),
             "Clearing pending interrupts disabled future timer delivery");
    clearAll();
}

} // namespace gem5::X86ISA
