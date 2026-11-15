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

#include "arch/x86/generated/decoder.hh"
#include "arch/x86/regs/float.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/utility.hh"
#include "sim/sim_exit.hh"

namespace gem5::X86ISA
{

void
InterruptTester::run(const std::string &scenario)
{
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
InterruptTester::checkTimer()
{
    panic_if(IRRV != 0xd0 ||
                 !getRegArrayBit(APIC_INTERRUPT_REQUEST_BASE, 0xd0),
             "Clearing pending interrupts disabled future timer delivery");
    clearAll();
}

} // namespace gem5::X86ISA
