# Copyright (c) 2026 Seaver Olson
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Exercise x86 interrupt and register-reset semantics without a guest."""

import argparse

import m5
from m5.objects import (
    X86ISA,
    X86O3CPU,
    AddrRange,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
    X86AtomicSimpleCPU,
    X86BareMetalWorkload,
    X86InterruptTester,
    X86MinorCPU,
    X86TimingSimpleCPU,
)

cpu_types = {
    "atomic": X86AtomicSimpleCPU,
    "timing": X86TimingSimpleCPU,
    "minor": X86MinorCPU,
    "o3": X86O3CPU,
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "scenario", choices=("clear", "init", "reset", "thread-reset")
)
parser.add_argument("--cpu", choices=cpu_types, default="atomic")
parser.add_argument("--num-cpus", type=int, choices=(1, 2), default=1)
args = parser.parse_args()
if args.scenario != "thread-reset" and (
    args.cpu != "atomic" or args.num_cpus != 1
):
    parser.error("Interrupt tests require one AtomicSimpleCPU")
cpu_class = cpu_types[args.cpu]
root = Root(full_system=True)
root.system = system = System(mem_mode=cpu_class.memory_mode())
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_ranges = [AddrRange("1MiB")]
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.cpu = [cpu_class(cpu_id=i) for i in range(args.num_cpus)]
apics = []
for i, cpu in enumerate(system.cpu):
    cpu.interrupts = [X86InterruptTester()]
    apic = cpu.interrupts[0]
    apics.append(apic)
    if args.scenario == "thread-reset":
        # A nondefault signature, distinct per context, must reach EDX.
        cpu.isa = [X86ISA(FamilyModelStepping=[0x306A9 + i, 0, 0, 0])]
    cpu.createThreads()
    cpu.icache_port = system.membus.cpu_side_ports
    cpu.dcache_port = system.membus.cpu_side_ports
    cpu.mmu.connectWalkerPorts(
        system.membus.cpu_side_ports, system.membus.cpu_side_ports
    )
    apic.pio = system.membus.mem_side_ports
    apic.int_requestor = system.membus.cpu_side_ports
    apic.int_responder = system.membus.mem_side_ports
system.workload = X86BareMetalWorkload()
m5.instantiate()
if args.scenario == "thread-reset":
    for apic in apics:
        apic.run("suspend")
    m5.simulate(10_000)
    for apic in apics:
        apic.run("seed-thread-reset")
    for i, apic in enumerate(apics):
        apic.run("thread-reset")
        for other in apics[i + 1 :]:
            other.run("check-thread-seed")
    m5.simulate(10_000)
    for apic in apics:
        apic.run("check-reset-tsc")
else:
    apic = apics[0]
    apic.run(args.scenario)
    if args.scenario == "clear":
        event = m5.simulate(1_000_000)
        assert event.getCause() == "interrupt timer observation"
        apic.checkTimer()
print("x86 interrupt semantics passed")
