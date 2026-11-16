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

#ifndef __TEST_OBJECTS_X86_INTERRUPT_TESTER_HH__
#define __TEST_OBJECTS_X86_INTERRUPT_TESTER_HH__

#include <array>
#include <string>

#include "arch/x86/interrupts.hh"
#include "params/X86InterruptTester.hh"

namespace gem5::X86ISA
{

// A test-only APIC subclass can inspect latched and in-service state without
// adding test accessors to the production interrupt controller.
class InterruptTester : public Interrupts
{
  private:
    Cycles resetCycle = Cycles(0);
    std::string activeScenario;
    unsigned resetAttempt = 0;

    void checkValue(const std::string &field, RegVal actual,
                    RegVal expected) const;
    void checkMiscReg(RegIndex index, RegVal expected) const;
    void
    checkApicRegs(const std::array<uint32_t, NUM_APIC_REGS> &expected) const;
    void checkInterruptState(Tick expiration, bool pending) const;

    void seedInterrupts();
    void checkClear();
    void checkEoi();
    void checkInit();
    void checkUnsupportedReset();
    void seedThreadReset();
    void checkThreadSeed();
    void checkThreadReset();
    void checkResetRegisters();
    void checkResetSegments();
    void checkResetExecutionState();
    void checkResetTsc();
    void checkDecode(unsigned size);

  public:
    explicit InterruptTester(const X86InterruptTesterParams &p) : Interrupts(p)
    {}

    void run(const std::string &scenario);
    void checkTimer();
};

} // namespace gem5::X86ISA

#endif
