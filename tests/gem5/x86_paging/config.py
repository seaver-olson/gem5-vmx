"""Disk-free x86 paging regression using real CPU and walker ports."""
import argparse
from pathlib import Path
import m5
from m5.objects import (
    AddrRange, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU,
    Root, SrcClockDomain, System, SystemXBar, VoltageDomain,
    SimpleMemory, X86FsWorkload, Cache,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cpu', choices=['atomic', 'timing', 'o3'], default='atomic')
parser.add_argument('--caches', action='store_true')
parser.add_argument('--pcid', action='store_true',
                    help='Advertise PCID for control-register tests')
parser.add_argument('--guest', choices=['vmx_operands', 'paging', 'legacy', 'pae', 'gdb'], default='paging')
parser.add_argument('--gdb-socket', help='Wait for a debugger on this Unix socket')
parser.add_argument('--fixture', action='store_true',
                    help='Inspect MMU results directly using a suspended CPU')
parser.add_argument('--checkpoint-at', type=int, default=0,
                    help='Drain and checkpoint after this many ticks')
parser.add_argument('--restore', type=Path)
parser.add_argument('--pae-checkpoint', action='store_true',
                    help='Checkpoint after PDPTE memory diverges from retained state')
parser.add_argument('--switch-to', choices=['atomic', 'timing', 'o3'])
parser.add_argument('--switch-at', type=int, default=6025000)
parser.add_argument('--transport', action='store_true',
                    help='Include shared transport and lifecycle fixture cases')
args = parser.parse_args()
args.fixture |= args.transport
root = Root(full_system=True)
root.system = system = System()
system.clk_domain = SrcClockDomain(clock='2GHz', voltage_domain=VoltageDomain())
system.mem_mode = 'atomic' if args.cpu == 'atomic' else 'timing'
system.mem_ranges = [AddrRange('64MiB')]
system.membus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency='30ns')
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.cpu = {'atomic': AtomicSimpleCPU, 'timing': TimingSimpleCPU,
              'o3': DerivO3CPU}[args.cpu]()
if args.caches:
    for name in ('icache', 'dcache'):
        cache = Cache(size='16KiB', assoc=2, tag_latency=2,
                      data_latency=2, response_latency=2,
                      mshrs=4, tgts_per_mshr=8)
        setattr(system, name, cache)
        cache.mem_side = system.membus.cpu_side_ports
        setattr(system.cpu, name + '_port', cache.cpu_side)
else:
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
# Deliberately share the TLB: a data fill must also enforce fetch permissions.
system.cpu.mmu.itb = system.cpu.mmu.dtb
if args.caches:
    system.walkcache = Cache(size='8KiB', assoc=4, tag_latency=1,
                             data_latency=1, response_latency=1,
                             mshrs=4, tgts_per_mshr=8)
    system.walkcache.mem_side = system.membus.cpu_side_ports
    system.cpu.mmu.dtb.walker.port = system.walkcache.cpu_side
else:
    system.cpu.mmu.dtb.walker.port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.createThreads()
if args.pcid:
    features = list(system.cpu.isa[0].FamilyModelStepping)
    features[3] = int(features[3]) | (1 << 17)
    system.cpu.isa[0].FamilyModelStepping = features
if args.switch_to:
    system.switch_cpu = {'atomic': AtomicSimpleCPU, 'timing': TimingSimpleCPU,
                         'o3': DerivO3CPU}[args.switch_to](
                             cpu_id=0, switched_out=True)
    system.switch_cpu.mmu.itb = system.switch_cpu.mmu.dtb
    system.switch_cpu.createThreads()
    if args.pcid:
        system.switch_cpu.isa[0].FamilyModelStepping = features
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
system.workload = X86FsWorkload(
    object_file=str(Path(__file__).with_name(args.guest + '.elf')),
    exit_on_kernel_panic=False,
    load_addr_mask=0xffffffffffffffff,
)
if args.gdb_socket:
    system.workload.remote_gdb_port = args.gdb_socket
    system.workload.wait_for_remote_gdb = True
input_file = Path(m5.options.outdir) / 'functional-input.bin'
input_file.write_bytes(bytes.fromhex('0011223344556677'))
system.readfile = str(input_file)
if args.fixture:
    if args.cpu == 'o3':
        parser.error('The direct fixture uses an atomic or timing context')
    from m5.objects import X86PagingTester
    system.paging_tester = X86PagingTester(system=system, transport_cases=args.transport)
    system.cpu.mmu.dtb.walker.port.splice(
        system.paging_tester.cpu_side, system.paging_tester.mem_side)
m5.instantiate(str(args.restore) if args.restore else None)
if args.checkpoint_at:
    event = m5.simulate(args.checkpoint_at)
    if event.getCause() != 'simulate() limit reached':
        raise RuntimeError(f'Unexpected pre-checkpoint exit: {event.getCause()}')
    m5.checkpoint(str(Path(m5.options.outdir) / 'checkpoint'))
if args.switch_to and args.guest != 'pae':
    event = m5.simulate(args.switch_at)
    if event.getCause() != 'simulate() limit reached':
        raise RuntimeError(f'Unexpected pre-switch exit: {event.getCause()}')
    m5.switchCpus(system, [(system.cpu, system.switch_cpu)])
# Each exit reports a test ID in its code. Zero is a complete pass.
event = m5.simulate(10_000_000_000)
if args.guest == 'pae' and event.getCause() == 'm5_exit instruction encountered':
    if args.pae_checkpoint:
        m5.checkpoint(str(Path(m5.options.outdir) / 'checkpoint'))
    if args.switch_to:
        m5.switchCpus(system, [(system.cpu, system.switch_cpu)])
    event = m5.simulate(10_000_000_000)
print(f'PAGING RESULT: {event.getCause()} code={event.getCode()}')
if args.fixture:
    raise SystemExit(0 if event.getCause() == 'paging fixture passed' else 1)
if event.getCause() != 'm5_fail instruction encountered' or event.getCode() != 0:
    raise SystemExit(event.getCode() or 1)

expected = bytes.fromhex('efcdab89674523011032547698badcfe')
if args.guest == 'paging':
    assert (Path(m5.options.outdir) / 'functional.bin').read_bytes() == expected
