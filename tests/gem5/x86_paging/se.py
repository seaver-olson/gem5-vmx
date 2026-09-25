"""Shared x86 TLB isolation across SE processes with colliding low PID bits."""
from pathlib import Path

import m5
from m5.objects import (
    AddrRange, Cache, DerivO3CPU, Process, Root, SEWorkload, SimpleMemory,
    SrcClockDomain, System, SystemXBar, VoltageDomain,
)

suite = Path(__file__).resolve().parent
system = System(mem_mode='timing', mem_ranges=[AddrRange('128MiB')],
                multi_thread=True)
system.clk_domain = SrcClockDomain(clock='2GHz', voltage_domain=VoltageDomain())
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.cpu = DerivO3CPU(numThreads=2)
for kind in ('icache', 'dcache'):
    cache = Cache(size='16KiB', assoc=2, tag_latency=2, data_latency=2,
                  response_latency=2, mshrs=4, tgts_per_mshr=8)
    setattr(system, kind, cache)
    cache.mem_side = system.membus.cpu_side_ports
    setattr(system.cpu, kind + '_port', cache.cpu_side)
system.cpu.mmu.connectWalkerPorts(system.membus.cpu_side_ports,
                                  system.membus.cpu_side_ports)
system.cpu.createInterruptController()
for interrupt in system.cpu.interrupts:
    interrupt.pio = system.membus.mem_side_ports
    interrupt.int_requestor = system.membus.cpu_side_ports
    interrupt.int_responder = system.membus.mem_side_ports
processes = []
for identity, pid in ((1, 100), (2, 4196)):
    binary = str(suite / f'se-context-{identity}')
    process = Process(pid=pid, cmd=[binary], executable=binary,
                      output=f'process-{identity}.out')
    processes.append(process)
system.cpu.workload = processes
system.workload = SEWorkload.init_compatible(processes[0].executable)
system.cpu.createThreads()
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate(10_000_000_000)
assert event.getCause() == 'exiting with last active thread context', event.getCause()
assert event.getCode() == 0, event.getCode()
for identity in (1, 2):
    output = Path(m5.options.outdir) / f'process-{identity}.out'
    assert output.read_text() == f'SE{identity}!\n', output.read_text()
print('SE CONTEXTS: PASS')
