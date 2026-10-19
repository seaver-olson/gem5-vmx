"""Disk-free x86 paging regression using real CPU and walker ports."""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    AtomicSimpleCPU,
    Cache,
    DerivO3CPU,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    TimingSimpleCPU,
    VoltageDomain,
    X86FsWorkload,
    X86MinorCPU,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--cpu", choices=["atomic", "timing", "o3", "minor"], default="atomic"
)
parser.add_argument("--caches", action="store_true")
parser.add_argument(
    "--direct-walker",
    action="store_true",
    help="Bypass the walker cache (raw coherent swaps are unsupported)",
)
parser.add_argument("--no-snoop-filter", action="store_true")
parser.add_argument(
    "--physical-bits",
    type=int,
    choices=[32, 40, 48, 52],
    default=48,
    help="Advertise this MAXPHYADDR through CPUID.80000008H",
)
parser.add_argument(
    "--pcid",
    action="store_true",
    help="Advertise PCID for control-register tests",
)
parser.add_argument(
    "--guest",
    choices=[
        "vmx_operands",
        "vmresume",
        "ept_entry",
        "vmx_transition",
        "vmx_nmi",
        "paging",
        "legacy",
        "pae",
        "gdb",
    ],
    default="paging",
)
parser.add_argument(
    "--vmresume-latency",
    type=int,
    help="Uncalibrated O3 VMRESUME execution latency in cycles",
)
parser.add_argument(
    "--vmlaunch-latency",
    type=int,
    help="Uncalibrated O3 VMLAUNCH execution latency in cycles",
)
parser.add_argument(
    "--gdb-socket", help="Wait for a debugger on this Unix socket"
)
parser.add_argument(
    "--fixture",
    action="store_true",
    help="Inspect MMU results directly using a suspended CPU",
)
parser.add_argument(
    "--checkpoint-at",
    type=int,
    default=0,
    help="Drain and checkpoint after this many ticks",
)
parser.add_argument("--restore", type=Path)
parser.add_argument(
    "--vmx-root-checkpoint",
    action="store_true",
    help="Checkpoint at the transition guest's VMX-root marker",
)
parser.add_argument(
    "--pae-checkpoint",
    action="store_true",
    help="Checkpoint after PDPTE memory diverges from retained state",
)
parser.add_argument("--switch-to", choices=["atomic", "timing", "o3"])
parser.add_argument("--switch-at", type=int, default=6025000)
parser.add_argument(
    "--transport",
    action="store_true",
    help="Include shared transport and lifecycle fixture cases",
)
parser.add_argument(
    "--ept-only",
    action="store_true",
    help="Run standalone EPT walker tests in all memory modes",
)
args = parser.parse_args()
if args.vmx_root_checkpoint and (
    args.guest != "vmx_transition"
    or args.restore
    or args.checkpoint_at
    or args.switch_to
    or args.fixture
    or args.transport
    or args.ept_only
):
    parser.error(
        "--vmx-root-checkpoint requires a fresh vmx_transition guest "
        "without other checkpoint, fixture or switching options"
    )
if args.ept_only and (
    args.cpu not in ("atomic", "timing")
    or args.transport
    or args.switch_to
    or args.restore
    or args.checkpoint_at
):
    parser.error(
        "--ept-only requires an atomic or timing context without "
        "transport, CPU switching, or checkpoints"
    )
for instruction in ("vmresume", "vmlaunch"):
    latency = getattr(args, instruction + "_latency")
    if latency is not None and (
        args.cpu != "o3" or args.switch_to or latency < 1
    ):
        parser.error(
            f"--{instruction}-latency requires O3, no CPU switch, "
            "and >= 1 cycle"
        )
args.fixture |= args.transport or args.ept_only
root = Root(full_system=True)
root.system = system = System()
system.clk_domain = SrcClockDomain(
    clock="2GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "atomic" if args.cpu == "atomic" else "timing"
system.mem_ranges = [AddrRange("64MiB")]
system.membus = SystemXBar()
if args.no_snoop_filter:
    from m5.params import NULL

    system.membus.snoop_filter = NULL
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="30ns")
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
system.cpu = {
    "atomic": AtomicSimpleCPU,
    "timing": TimingSimpleCPU,
    "o3": DerivO3CPU,
    "minor": X86MinorCPU,
}[args.cpu]()
if args.cpu == "o3":
    latencies = {
        "VmxResume": args.vmresume_latency,
        "VmxLaunch": args.vmlaunch_latency,
    }
    for unit in system.cpu.instQueues[0].fuPool.FUList:
        for op in unit.opList:
            latency = latencies.get(str(op.opClass))
            if latency is not None:
                op.opLat = latency
if args.caches:
    for name in ("icache", "dcache"):
        cache = Cache(
            size="16KiB",
            assoc=2,
            tag_latency=2,
            data_latency=2,
            response_latency=2,
            mshrs=4,
            tgts_per_mshr=8,
        )
        setattr(system, name, cache)
        cache.mem_side = system.membus.cpu_side_ports
        setattr(system.cpu, name + "_port", cache.cpu_side)
else:
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
# Deliberately share the TLB: a data fill must also enforce fetch permissions.
system.cpu.mmu.itb = system.cpu.mmu.dtb
if args.caches and not args.direct_walker:
    system.walkcache = Cache(
        size="8KiB",
        assoc=4,
        tag_latency=1,
        data_latency=1,
        response_latency=1,
        mshrs=4,
        tgts_per_mshr=8,
    )
    system.walkcache.mem_side = system.membus.cpu_side_ports
    system.cpu.mmu.dtb.walker.port = system.walkcache.cpu_side
else:
    system.cpu.mmu.dtb.walker.port = system.membus.cpu_side_ports
system.cpu.createInterruptController()
system.cpu.createThreads()
address_sizes = list(system.cpu.isa[0].LongModeAddressSize)
address_sizes[0] = (int(address_sizes[0]) & ~0xFF) | args.physical_bits
system.cpu.isa[0].LongModeAddressSize = address_sizes
if args.pcid:
    features = list(system.cpu.isa[0].FamilyModelStepping)
    features[3] = int(features[3]) | (1 << 17)
    system.cpu.isa[0].FamilyModelStepping = features
if args.switch_to:
    system.switch_cpu = {
        "atomic": AtomicSimpleCPU,
        "timing": TimingSimpleCPU,
        "o3": DerivO3CPU,
    }[args.switch_to](cpu_id=0, switched_out=True)
    system.switch_cpu.mmu.itb = system.switch_cpu.mmu.dtb
    system.switch_cpu.createThreads()
    system.switch_cpu.isa[0].LongModeAddressSize = address_sizes
    if args.pcid:
        system.switch_cpu.isa[0].FamilyModelStepping = features
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports
system.workload = X86FsWorkload(
    object_file=str(Path(__file__).with_name(args.guest + ".elf")),
    exit_on_kernel_panic=False,
    load_addr_mask=0xFFFFFFFFFFFFFFFF,
)
if args.gdb_socket:
    system.workload.remote_gdb_port = args.gdb_socket
    system.workload.wait_for_remote_gdb = True
input_file = Path(m5.options.outdir) / "functional-input.bin"
input_file.write_bytes(bytes.fromhex("0011223344556677"))
system.readfile = str(input_file)
if args.fixture:
    if args.cpu not in ("atomic", "timing"):
        parser.error("The direct fixture uses an atomic or timing context")
    from m5.objects import X86PagingTester

    system.paging_tester = X86PagingTester(
        system=system, transport_cases=args.transport, ept_only=args.ept_only
    )
    system.cpu.mmu.dtb.walker.port.splice(
        system.paging_tester.cpu_side, system.paging_tester.mem_side
    )
m5.instantiate(str(args.restore) if args.restore else None)
if args.checkpoint_at:
    event = m5.simulate(args.checkpoint_at)
    if event.getCause() != "simulate() limit reached":
        raise RuntimeError(
            f"Unexpected pre-checkpoint exit: {event.getCause()}"
        )
    m5.checkpoint(str(Path(m5.options.outdir) / "checkpoint"))
if args.switch_to and args.guest != "pae":
    event = m5.simulate(args.switch_at)
    if event.getCause() != "simulate() limit reached":
        raise RuntimeError(f"Unexpected pre-switch exit: {event.getCause()}")
    m5.switchCpus(system, [(system.cpu, system.switch_cpu)])
# Each exit reports a test ID in its code. Zero is a complete pass.
event = m5.simulate(10_000_000_000)
if args.guest == "vmx_transition" and not args.restore:
    if event.getCause() != "m5_exit instruction encountered":
        raise RuntimeError(
            f"Missing VMX-root marker: {event.getCause()} "
            f"code={event.getCode()}"
        )
    print("VMX ROOT MARKER: current VMCS and guest RIP set")
    if args.vmx_root_checkpoint:
        m5.checkpoint(str(Path(m5.options.outdir) / "checkpoint"))
    event = m5.simulate(10_000_000_000)
if (
    args.guest == "pae"
    and event.getCause() == "m5_exit instruction encountered"
):
    if args.pae_checkpoint:
        m5.checkpoint(str(Path(m5.options.outdir) / "checkpoint"))
    if args.switch_to:
        m5.switchCpus(system, [(system.cpu, system.switch_cpu)])
    event = m5.simulate(10_000_000_000)
print(f"PAGING RESULT: {event.getCause()} code={event.getCode()}")
if args.fixture:
    raise SystemExit(0 if event.getCause() == "paging fixture passed" else 1)
if (
    event.getCause() != "m5_fail instruction encountered"
    or event.getCode() != 0
):
    raise SystemExit(event.getCode() or 1)

expected = bytes.fromhex("efcdab89674523011032547698badcfe")
if args.guest == "paging":
    assert (
        Path(m5.options.outdir) / "functional.bin"
    ).read_bytes() == expected
