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
    activeScenario = scenario;
    resetAttempt = 0;
    if (scenario == "suspend") {
        tc->suspend();
    } else if (scenario == "seed-thread-reset") {
        seedThreadReset();
    } else if (scenario == "thread-reset") {
        checkThreadReset();
    } else if (scenario == "check-thread-seed") {
        checkThreadSeed();
    } else if (scenario == "check-reset-tsc") {
        checkResetTsc();
    } else {
        // Keep timer/message delivery from waking a CPU into unmapped code.
        tc->halt();
        if (scenario == "clear") {
            checkClear();
        } else if (scenario == "eoi") {
            checkEoi();
        } else if (scenario == "init") {
            checkInit();
        } else if (scenario == "reset") {
            checkUnsupportedReset();
        } else {
            panic("Unknown interrupt test: %s", scenario);
        }
    }
}

void
InterruptTester::checkValue(const std::string &field, RegVal actual,
                            RegVal expected) const
{
    panic_if(actual != expected,
             "%s: scenario=%s context=%d reset-attempt=%u: "
             "%s: expected %#x, got %#x",
             name(), activeScenario, tc->contextId(), resetAttempt, field,
             expected, actual);
}

void
InterruptTester::checkMiscReg(RegIndex index, RegVal expected) const
{
    checkValue(csprintf("misc register %u", index),
               tc->readMiscRegNoEffect(index), expected);
}

void
InterruptTester::checkApicRegs(
    const std::array<uint32_t, NUM_APIC_REGS> &expected) const
{
    for (unsigned i = 0; i < expected.size(); ++i) {
        checkValue(csprintf("APIC register %u", i), regs[i], expected[i]);
    }
}

void
InterruptTester::checkInterruptState(Tick expiration, bool pending) const
{
    checkValue("ISRV", ISRV, 64);
    checkValue("IRRV", IRRV, pending ? 97 : 0);
    checkValue("startedUp", startedUp, true);
    checkValue("synthetic pendingIPIs", pendingIPIs, 1);
    checkValue("timer scheduled", apicTimerEvent.scheduled(), true);
    checkValue("timer expiration", apicTimerEvent.when(), expiration);
    checkValue("pendingSmi", pendingSmi, pending);
    checkValue("pendingNmi", pendingNmi, pending);
    checkValue("pendingExtInt", pendingExtInt, pending);
    checkValue("pendingInit", pendingInit, pending);
    checkValue("pendingStartup", pendingStartup, pending);
    checkValue("pendingUnmaskableInt", pendingUnmaskableInt, pending);
    const RegVal vector = pending ? 7 : 0;
    checkValue("smiVector", smiVector, vector);
    checkValue("nmiVector", nmiVector, vector);
    checkValue("extIntVector", extIntVector, vector);
    checkValue("initVector", initVector, vector);
    checkValue("startupVector", startupVector, vector);
    checkValue("checkInterruptsRaw", checkInterruptsRaw(), pending);
}

void
InterruptTester::checkClear()
{
    seedInterrupts();
    const Tick expiration = apicTimerEvent.when();
    std::array<uint32_t, NUM_APIC_REGS> expected;
    std::copy(std::begin(regs), std::end(regs), expected.begin());
    expected[APIC_INTERRUPT_REQUEST(2)] = 0;
    expected[APIC_INTERRUPT_REQUEST(3)] = 0;
    expected[APIC_TRIGGER_MODE(3)] = 0;

    clearAll();
    checkApicRegs(expected);
    checkInterruptState(expiration, false);

    // The generic fallback must still dispatch clearAll() for controllers
    // that do not implement a distinct reset operation.
    activeScenario = "clear/generic-reset-fallback";
    pendingNmi = pendingUnmaskableInt = true;
    BaseInterrupts::reset();
    checkApicRegs(expected);
    checkInterruptState(expiration, false);

    // No IPI was sent: remove synthetic bookkeeping without claiming to
    // exercise the message path or completeIPI() response callback.
    pendingIPIs = 0;
    regs[APIC_INTERRUPT_COMMAND_LOW] = 0;
    exitSimulationLoopClassic("interrupt timer observation", 0,
                              expiration + 1);
}

void
InterruptTester::checkEoi()
{
    setRFlags(tc, 0x202);
    requestInterrupt(64, delivery_mode::Fixed, true);
    checkValue("initial level request deliverable", checkInterrupts(), true);
    updateIntrInfo();
    requestInterrupt(64, delivery_mode::Fixed, true);
    requestInterrupt(97, delivery_mode::Fixed, true);
    checkValue("ISR[64] before clear",
               getRegArrayBit(APIC_IN_SERVICE_BASE, 64), true);
    checkValue("IRR[64] before clear",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 64), true);
    checkValue("TMR[64] before clear",
               getRegArrayBit(APIC_TRIGGER_MODE_BASE, 64), true);
    checkValue("IRR[97] before clear",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 97), true);
    checkValue("TMR[97] before clear",
               getRegArrayBit(APIC_TRIGGER_MODE_BASE, 97), true);

    clearAll();
    checkValue("ISR[64] after clear", getRegArrayBit(APIC_IN_SERVICE_BASE, 64),
               true);
    checkValue("ISRV after clear", ISRV, 64);
    checkValue("IRR[64] after clear",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 64), false);
    checkValue("TMR[64] after clear",
               getRegArrayBit(APIC_TRIGGER_MODE_BASE, 64), true);
    checkValue("IRR[97] after clear",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 97), false);
    checkValue("TMR[97] after clear",
               getRegArrayBit(APIC_TRIGGER_MODE_BASE, 97), false);
    checkValue("IRRV after clear", IRRV, 0);
    checkValue("request deliverable after clear", checkInterruptsRaw(), false);

    setReg(APIC_EOI, 0);
    checkValue("ISR[64] after EOI", getRegArrayBit(APIC_IN_SERVICE_BASE, 64),
               false);
    checkValue("ISRV after EOI", ISRV, 0);
    checkValue("IRR[64] after EOI",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 64), false);
    checkValue("IRRV after EOI", IRRV, 0);
    checkValue("discarded request resurrected by EOI", checkInterruptsRaw(),
               false);

    // gem5's EOI retires ISR state; it does not broadcast an IOAPIC EOI.
    // A fresh edge request must replace the old level-trigger metadata.
    requestInterrupt(64, delivery_mode::Fixed, false);
    checkValue("new edge request deliverable", checkInterrupts(), true);
    checkValue("IRR[64] for new edge request",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 64), true);
    checkValue("TMR[64] for new edge request",
               getRegArrayBit(APIC_TRIGGER_MODE_BASE, 64), false);
    updateIntrInfo();
    checkValue("ISR[64] for new edge request",
               getRegArrayBit(APIC_IN_SERVICE_BASE, 64), true);
    setReg(APIC_EOI, 0);
    checkValue("ISRV after edge EOI", ISRV, 0);
    checkValue("request deliverable after edge EOI", checkInterruptsRaw(),
               false);
}

void
InterruptTester::checkInit()
{
    checkMiscReg(misc_reg::Mxcsr, 0x1f80);
    checkMiscReg(misc_reg::Ftw, 0xffff);
    const std::array<RegIndex, 5> indices = {misc_reg::Mxcsr, misc_reg::Ftw,
                                             misc_reg::Fcw, misc_reg::Fsw,
                                             misc_reg::X87Top};
    const std::array<RegVal, 5> values = {0x3f80, 0xaaaa, 0x077f, 0x0240, 3};
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
        checkMiscReg(indices[i], values[i]);
    }
    for (unsigned i = 0; i < float_reg::MicrofpBase; ++i) {
        checkValue(csprintf("FP register %u", i), tc->getReg(floatRegClass[i]),
                   0x12340000 + i);
    }
    const auto &pc = tc->pcState().as<PCState>();
    const auto halt_pc =
        romMicroPC(X86ISAInst::rom_labels::extern_label_initIntHalt);
    checkValue("RAX", tc->getReg(int_reg::Rax), 0);
    checkMiscReg(misc_reg::Cr0, 0x20000010);
    checkMiscReg(misc_reg::Cr3, 0);
    checkMiscReg(misc_reg::Cr4, 0);
    checkMiscReg(misc_reg::Cs, 0xf000);
    checkValue("PC", pc.instAddr(), 0xfffffff0);
    checkValue("wait-for-SIPI microPC", pc.microPC(), halt_pc);
}

void
InterruptTester::checkUnsupportedReset()
{
    reset();
    panic("Unsupported APIC RESET returned");
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
    // Synthetic outgoing IPI accounting only. No packet or completion
    // callback is exercised by this fixture.
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
    checkValue("thread status before seeding", tc->status(),
               ThreadContext::Suspended);
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
InterruptTester::checkThreadSeed()
{
    checkValue("unreset RAX", tc->getReg(int_reg::Rax), 0x12340000);
    checkMiscReg(misc_reg::Cr0, 0x80000011);
    checkValue("unreset FP register 0", tc->getReg(flatFloatRegClass[0]),
               0x56780000);
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
    checkValue("decoder fetch width", decoder->moreBytesSize(), bytes.size());
    std::memcpy(decoder->moreBytesPtr(), bytes.data(), bytes.size());
    PCState pc(tc->pcState().instAddr());
    decoder->moreBytes(pc, pc.instAddr());
    checkValue("decoder instruction ready", decoder->instReady(), true);
    checkValue("decoder returned an instruction", bool(decoder->decode(pc)),
               true);
    checkValue("decoded instruction size", pc.size(), size);
    decoder->reset();
}

void
InterruptTester::checkThreadReset()
{
    checkValue("nonzero reset cycle", bool(tc->getCpuPtr()->curCycle()), true);
    seedInterrupts();
    std::array<uint32_t, NUM_APIC_REGS> expected_apic;
    std::copy(std::begin(regs), std::end(regs), expected_apic.begin());
    const Tick expiration = apicTimerEvent.when();
    const auto status = tc->status();

    // Repeat without reseeding to check that reset is idempotent.
    for (resetAttempt = 1; resetAttempt <= 2; ++resetAttempt) {
        tc->getIsaPtr()->resetThread();
        resetCycle = tc->getCpuPtr()->curCycle();
        checkResetRegisters();
        checkResetSegments();
        checkResetExecutionState();
        checkValue("thread status after reset", tc->status(), status);
        checkDecode(3);
        checkApicRegs(expected_apic);
        checkInterruptState(expiration, true);
    }
    // Remove synthetic requests before advancing time to check TSC.
    deschedule(apicTimerEvent);
    pendingIPIs = 0;
    regs[APIC_INTERRUPT_COMMAND_LOW] = 0;
    clearAll();
}

void
InterruptTester::checkResetRegisters()
{
    for (RegIndex i = 0; i < int_reg::NumRegs; ++i) {
        const RegVal expected =
            i == int_reg::_RdxIdx ? 0x306a9 + tc->contextId() : 0;
        checkValue(csprintf("integer register %u", i),
                   tc->getReg(flatIntRegClass[i]), expected);
    }
    for (RegIndex i = 0; i < float_reg::NumRegs; ++i) {
        checkValue(csprintf("FP register %u", i),
                   tc->getReg(flatFloatRegClass[i]), 0);
    }
    for (RegIndex i = 0; i < cc_reg::NumRegs; ++i) {
        checkValue(csprintf("CC register %u", i), tc->getReg(ccRegClass[i]),
                   0);
    }
    const std::pair<RegIndex, RegVal> expected_misc[] = {
        {misc_reg::Cr0, 0x60000010}, {misc_reg::Cr2, 0},
        {misc_reg::Cr3, 0},          {misc_reg::Cr4, 0},
        {misc_reg::Cr8, 0},          {misc_reg::Efer, 0},
        {misc_reg::Xcr0, 1},         {misc_reg::Dr0, 0},
        {misc_reg::Dr1, 0},          {misc_reg::Dr2, 0},
        {misc_reg::Dr3, 0},          {misc_reg::Dr6, 0xffff0ff0},
        {misc_reg::Dr7, 0x400},      {misc_reg::Fcw, 0x40},
        {misc_reg::Ftw, 0x5555},     {misc_reg::Mxcsr, 0x1f80},
        {misc_reg::Fsw, 0},          {misc_reg::X87Top, 0},
        {misc_reg::Ftag, 0},         {misc_reg::Fiseg, 0},
        {misc_reg::Fioff, 0},        {misc_reg::Foseg, 0},
        {misc_reg::Fooff, 0},        {misc_reg::Fop, 0},
        {misc_reg::DefType, 0},      {misc_reg::Pat, 0x0007040600070406ULL},
        {misc_reg::Mtrrcap, 0x508},  {misc_reg::McgCap, 0x104},
        {misc_reg::SysenterCs, 0},   {misc_reg::SysenterEsp, 0},
        {misc_reg::SysenterEip, 0},  {misc_reg::Star, 0},
        {misc_reg::Lstar, 0},        {misc_reg::TscAux, 0}};
    for (const auto &[index, value] : expected_misc) {
        checkMiscReg(index, value);
    }
    for (RegIndex i = misc_reg::MtrrPhysBaseBase; i < misc_reg::Pat; ++i) {
        checkMiscReg(i, 0);
    }
}

void
InterruptTester::checkResetSegments()
{
    for (int seg = 0; seg < segment_idx::NumIdxs; ++seg) {
        const RegVal base = seg == segment_idx::Cs ? 0xffff0000 : 0;
        const RegVal selector = seg == segment_idx::Cs ? 0xf000 : 0;
        checkMiscReg(misc_reg::segBase(seg), base);
        checkMiscReg(misc_reg::segEffBase(seg), base);
        checkMiscReg(misc_reg::segSel(seg), selector);
        checkMiscReg(misc_reg::segLimit(seg), 0xffff);
        if (seg <= segment_idx::Gs) {
            const SegAttr attr =
                tc->readMiscRegNoEffect(misc_reg::segAttr(seg));
            const bool code = seg == segment_idx::Cs;
            const auto check_attr = [&](const char *field, RegVal actual,
                                        RegVal expected) {
                checkValue(csprintf("segment %d %s", seg, field), actual,
                           expected);
            };
            check_attr("dpl", attr.dpl, 0);
            check_attr("unusable", attr.unusable, 0);
            check_attr("defaultSize", attr.defaultSize, 0);
            check_attr("longMode", attr.longMode, 0);
            check_attr("avl", attr.avl, 0);
            check_attr("granularity", attr.granularity, 0);
            check_attr("present", attr.present, 1);
            check_attr("type", attr.type, code ? 11 : 3);
            check_attr("writable", attr.writable, !code);
            check_attr("readable", attr.readable, 1);
            check_attr("expandDown", attr.expandDown, 0);
            check_attr("system", attr.system, 1);
        }
    }
    const SegAttr ldt = tc->readMiscRegNoEffect(misc_reg::TslAttr);
    const SegAttr tr = tc->readMiscRegNoEffect(misc_reg::TrAttr);
    checkValue("LDT unusable", ldt.unusable, 1);
    checkValue("LDT present", ldt.present, 1);
    checkValue("LDT type", ldt.type, 2);
    checkValue("LDT system", ldt.system, 0);
    checkValue("TR unusable", tr.unusable, 0);
    checkValue("TR present", tr.present, 1);
    checkValue("TR type", tr.type, 3);
    checkValue("TR system", tr.system, 0);
}

void
InterruptTester::checkResetExecutionState()
{
    const HandyM5Reg mode = tc->readMiscRegNoEffect(misc_reg::M5Reg);
    const auto &pc = tc->pcState().as<PCState>();
    const LocalApicBase apic_base = tc->readMiscReg(misc_reg::ApicBase);
    checkValue("RFLAGS", getRFlags(tc), 2);
    checkValue("TSC at reset instant", tc->readMiscReg(misc_reg::Tsc), 0);
    checkValue("execution mode", mode.mode, LegacyMode);
    checkValue("execution submode", mode.submode, RealMode);
    checkValue("CPL", mode.cpl, 0);
    checkValue("paging", mode.paging, 0);
    checkValue("protected mode", mode.prot, 0);
    checkValue("default operand size", mode.defOp, 1);
    checkValue("default address size", mode.defAddr, 1);
    checkValue("stack size", mode.stack, 1);
    checkValue("PC", pc.instAddr(), 0xfffffff0);
    checkValue("microPC", pc.upc(), 0);
    checkValue("next microPC", pc.nupc(), 1);
    checkValue("PC instruction size", pc.size(), 0);
    checkValue("next PC", pc.npc(), 0xfffffff8);
    checkValue("APIC base", apic_base.base, 0xfee00);
    checkValue("APIC enabled", apic_base.enable, 1);
    checkValue("APIC BSP bit", apic_base.bsp, tc->contextId() == 0);
}

void
InterruptTester::checkResetTsc()
{
    const auto elapsed = tc->getCpuPtr()->curCycle() - resetCycle;
    checkValue("elapsed cycles after reset", bool(elapsed), true);
    checkValue("TSC after advancing time", tc->readMiscReg(misc_reg::Tsc),
               elapsed);
}

void
InterruptTester::checkTimer()
{
    activeScenario = "clear/timer-delivery";
    checkValue("IRRV after timer delivery", IRRV, 0xd0);
    checkValue("IRR[208] after timer delivery",
               getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 0xd0), true);
    clearAll();
}

} // namespace gem5::X86ISA
