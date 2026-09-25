"""Disk-free guest-paging test under timing Ruby MESI Two Level."""
import argparse
from pathlib import Path
import m5
from m5.objects import X86FsWorkload
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cpu', choices=['atomic', 'timing', 'o3'], default='timing')
parser.add_argument('--classic', choices=['l1', 'private-l2', 'shared-l2'])
parser.add_argument('--guest', choices=['vmx_operands', 'paging', 'legacy', 'pae'], default='paging')
parser.add_argument('--fixture', action='store_true')
parser.add_argument('--fixture-peers', action='store_true',
                    help='Exercise concurrent updates from two CPU walkers')
parser.add_argument('--transport', action='store_true',
                    help='Include shared transport and lifecycle fixture cases')
args = parser.parse_args()
args.fixture |= args.transport
args.fixture |= args.fixture_peers
if args.classic:
    from gem5.components.cachehierarchies.classic.private_l1_cache_hierarchy import PrivateL1CacheHierarchy
    from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import PrivateL1PrivateL2CacheHierarchy
    from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy import PrivateL1SharedL2CacheHierarchy
    constructors = {'l1': PrivateL1CacheHierarchy,
                    'private-l2': PrivateL1PrivateL2CacheHierarchy,
                    'shared-l2': PrivateL1SharedL2CacheHierarchy}
    options = {'l1i_size': '16KiB', 'l1d_size': '16KiB'}
    if args.classic != 'l1':
        options['l2_size'] = '256KiB'
    hierarchy = constructors[args.classic](**options)
else:
    if args.cpu == 'atomic':
        parser.error('Ruby requires timing or O3')
    hierarchy = MESITwoLevelCacheHierarchy(
        l1i_size='16KiB', l1i_assoc=2, l1d_size='16KiB', l1d_assoc=2,
        l2_size='256KiB', l2_assoc=8, num_l2_banks=1,
    )
board = X86Board(
    clk_freq='2GHz',
    processor=SimpleProcessor(
        cpu_type={'atomic': CPUTypes.ATOMIC, 'timing': CPUTypes.TIMING,
                  'o3': CPUTypes.O3}[args.cpu],
        isa=ISA.X86, num_cores=2 if args.fixture_peers else 1,
    ),
    memory=SingleChannelDDR3_1600(size='64MiB'),
    cache_hierarchy=hierarchy,
)
# This intentionally supplies a bare ELF instead of the board's Linux/disk
# workload helper. All cache and walker connections use the standard board.
board._set_fullsystem(True)
board.workload = X86FsWorkload(
    object_file=str(Path(__file__).with_name(args.guest + '.elf')),
    exit_on_kernel_panic=False, load_addr_mask=0xffffffffffffffff,
)
input_file = Path(m5.options.outdir) / 'functional-input.bin'
input_file.write_bytes(bytes.fromhex('0011223344556677'))
board.readfile = str(input_file)
root = board._pre_instantiate()
if args.fixture:
    if args.cpu == 'o3':
        parser.error('The direct fixture uses an atomic or timing context')
    from m5.objects import X86PagingTester
    board.paging_tester = X86PagingTester(system=board, transport_cases=args.transport)
    cpu = board.get_processor().get_cores()[0].get_simobject()
    cpu.mmu.dtb.walker.port.splice(
        board.paging_tester.cpu_side, board.paging_tester.mem_side)
m5.instantiate()
event = m5.simulate(10_000_000_000)
if args.guest == 'pae' and event.getCause() == 'm5_exit instruction encountered':
    event = m5.simulate(10_000_000_000)
print(f'PAGING RESULT: {event.getCause()} code={event.getCode()}')
if args.fixture:
    raise SystemExit(0 if event.getCause() == 'paging fixture passed' else 1)
if event.getCause() != 'm5_fail instruction encountered' or event.getCode() != 0:
    raise SystemExit(event.getCode() or 1)
expected = bytes.fromhex('efcdab89674523011032547698badcfe')
if args.guest == 'paging':
    assert (Path(m5.options.outdir) / 'functional.bin').read_bytes() == expected
