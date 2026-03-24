# =============================================================================
# vmx_research.py
# gem5 Full System Configuration for VMX/EPT Research
#
# CPU:     TimingSimpleCPU (medium accuracy, switch to O3 when ready)
# ISA:     x86
# Mode:    Full System (FS) — required for VMX/hypervisor work
# Memory:  DDR4 2400, 3GB default
#
# Usage:
#   gem5.opt vmx_research.py
#   gem5.opt vmx_research.py --mem-size=4GB
#   gem5.opt vmx_research.py --num-cpus=2
#   gem5.opt vmx_research.py --cpu-type=O3CPU   # when ready to bump up
#
# Switching to O3:
#   Simply pass --cpu-type=O3CPU on the command line, no other changes needed.
#   The script handles both CPU types identically from this point down.
# =============================================================================

from __future__ import print_function
from __future__ import absolute_import

import argparse
import sys
import os

import m5
from m5.objects import *
from m5.util import addToPath, fatal

# ---------------------------------------------------------------------------
# Path setup — adjust GEM5_PATH if running from outside the gem5 root
# ---------------------------------------------------------------------------
GEM5_PATH = os.environ.get("GEM5_PATH", os.path.dirname(os.path.abspath(__file__)) + "/..")
addToPath(os.path.join(GEM5_PATH, "configs"))

from common import Options
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import ObjectList
from common import MemConfig
from common.FileSystemConfig import config_filesystem
from topologies import *

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="gem5 Full System config for VMX/EPT research"
)

# CPU
parser.add_argument(
    "--cpu-type",
    type=str,
    default="TimingSimpleCPU",
    choices=["TimingSimpleCPU", "O3CPU", "AtomicSimpleCPU"],
    help="CPU model to use. Default: TimingSimpleCPU. "
         "Switch to O3CPU when testing is complete.",
)
parser.add_argument(
    "--num-cpus",
    type=int,
    default=1,
    help="Number of CPUs (default: 1). Keep at 1 during early VMX dev.",
)

# Memory
parser.add_argument(
    "--mem-size",
    type=str,
    default="3GB",
    help="System memory size (default: 3GB).",
)
parser.add_argument(
    "--mem-type",
    type=str,
    default="DDR4_2400_8x8",
    help="Memory type (default: DDR4_2400_8x8).",
)

# Disk / Kernel — set defaults to env vars or common gem5 FS paths
parser.add_argument(
    "--kernel",
    type=str,
    default=os.environ.get(
        "M5_PATH_KERNEL",
        os.path.join(GEM5_PATH, "vmlinux")
    ),
    help="Path to Linux kernel binary (vmlinux).",
)
parser.add_argument(
    "--disk-image",
    type=str,
    default=os.environ.get(
        "M5_PATH_DISK",
        os.path.join(GEM5_PATH, "disk-image/ubuntu-18.04.img")
    ),
    help="Path to disk image.",
)

# Caches
parser.add_argument(
    "--caches",
    action="store_true",
    default=True,
    help="Enable L1 caches (default: on).",
)
parser.add_argument(
    "--l2cache",
    action="store_true",
    default=True,
    help="Enable L2 cache (default: on).",
)
parser.add_argument(
    "--l1d-size",
    type=str,
    default="32kB",
    help="L1 data cache size (default: 32kB).",
)
parser.add_argument(
    "--l1i-size",
    type=str,
    default="32kB",
    help="L1 instruction cache size (default: 32kB).",
)
parser.add_argument(
    "--l2-size",
    type=str,
    default="256kB",
    help="L2 cache size (default: 256kB).",
)

# VMX research flags
parser.add_argument(
    "--vmx-debug",
    action="store_true",
    default=False,
    help="Enable verbose VMX debug output. "
         "Use when testing VMXON/VMLAUNCH/exits.",
)
parser.add_argument(
    "--ept-debug",
    action="store_true",
    default=False,
    help="Enable verbose EPT walker debug output. "
         "Enable after VMX exits are working.",
)

# Simulation control
parser.add_argument(
    "--max-tick",
    type=int,
    default=None,
    help="Stop simulation after this many ticks. "
         "Useful for short VMX instruction tests.",
                    )
parser.add_argument(
    "--checkpoint-dir",
    type=str,
    default="vmx_checkpoints",
    help="Directory for checkpoints (default: vmx_checkpoints). "
         "Use checkpoints to skip past boot when iterating on VMX code.",
)
parser.add_argument(
    "--restore-checkpoint",
    type=str,
    default=None,
    help="Restore from a checkpoint directory before starting sim. "
         "Recommended workflow: checkpoint after boot, restore for each VMX test.",
)

args = parser.parse_args()

# ---------------------------------------------------------------------------
# Validate paths
# ---------------------------------------------------------------------------
if not os.path.exists(args.kernel):
    fatal(
        "Kernel not found at: %s\n"
        "Set --kernel or export M5_PATH_KERNEL=<path>" % args.kernel
    )

if not os.path.exists(args.disk_image):
    fatal(
        "Disk image not found at: %s\n"
        "Set --disk-image or export M5_PATH_DISK=<path>" % args.disk_image
    )

# ---------------------------------------------------------------------------
# CPU selection
# ---------------------------------------------------------------------------
if args.cpu_type == "TimingSimpleCPU":
    CPUClass = TimingSimpleCPU
    print("[vmx_research] CPU: TimingSimpleCPU (medium accuracy)")
elif args.cpu_type == "O3CPU":
    CPUClass = O3CPU
    print("[vmx_research] CPU: O3CPU (high accuracy, slow)")
elif args.cpu_type == "AtomicSimpleCPU":
    CPUClass = AtomicSimpleCPU
    print("[vmx_research] CPU: AtomicSimpleCPU (fast, no timing)")
else:
    fatal("Unknown CPU type: %s" % args.cpu_type)

# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------
system = System()

system.clk_domain     = SrcClockDomain()
system.clk_domain.clock = "3GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"   # always timing — required for TimingSimpleCPU/O3
system.mem_ranges = [AddrRange(args.mem_size)]

# ---------------------------------------------------------------------------
# CPU instantiation
# ---------------------------------------------------------------------------
system.cpu = [CPUClass() for _ in range(args.num_cpus)]

# ---------------------------------------------------------------------------
# Caches
# ---------------------------------------------------------------------------
if args.caches:
    for cpu in system.cpu:
        cpu.icache = Cache(
            size=args.l1i_size,
            assoc=8,
            tag_latency=2,
            data_latency=2,
            response_latency=2,
            mshrs=4,
            tgts_per_mshr=20,
        )
        cpu.dcache = Cache(
            size=args.l1d_size,
            assoc=8,
            tag_latency=2,
            data_latency=2,
            response_latency=2,
            mshrs=4,
            tgts_per_mshr=20,
        )
        cpu.icache_port = cpu.icache.cpu_side
        cpu.dcache_port = cpu.dcache.cpu_side

        cpu.icache.mem_side = system.membus.cpu_side_ports
        cpu.dcache.mem_side = system.membus.cpu_side_ports

if args.l2cache:
    system.l2 = Cache(
        size=args.l2_size,
        assoc=16,
        tag_latency=20,
        data_latency=20,
        response_latency=20,
        mshrs=20,
        tgts_per_mshr=12,
    )
    system.l2bus = L2XBar()
    # reconnect L1s to L2 bus if caches are on
    if args.caches:
        for cpu in system.cpu:
            cpu.icache.mem_side = system.l2bus.cpu_side_ports
            cpu.dcache.mem_side = system.l2bus.cpu_side_ports
    system.l2bus.mem_side_ports = system.l2.cpu_side
    system.l2.mem_side = system.membus.cpu_side_ports

# ---------------------------------------------------------------------------
# Memory bus and interrupt controllers
# ---------------------------------------------------------------------------
system.membus = SystemXBar()
system.system_port = system.membus.cpu_side_ports

for cpu in system.cpu:
    cpu.createInterruptController()
    cpu.interrupts[0].pio     = system.membus.mem_side_ports
    cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
    cpu.interrupts[0].int_responder = system.membus.mem_side_ports

# ---------------------------------------------------------------------------
# Memory controller
# ---------------------------------------------------------------------------
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# ---------------------------------------------------------------------------
# x86 PC platform (required for FS)
# ---------------------------------------------------------------------------
system.pc = Pc()
system.pc.attachIO(system.iobus)

system.iobus = IOXBar()
system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port  = system.iobus.cpu_side_ports
system.bridge.cpu_side_port  = system.membus.mem_side_ports
system.bridge.ranges         = system.pc.address_ranges

system.pc.south_bridge.ide.disks = [
    CowDisk(args.disk_image)
]

# ---------------------------------------------------------------------------
# Kernel and boot
# ---------------------------------------------------------------------------
system.kernel = args.kernel

# Boot flags — enable VMX in the guest kernel if it supports it
# "nokaslr" makes debugging deterministic
# "console=ttyS0" routes output to gem5's serial terminal
system.boot_osflags = (
    "earlyprintk=ttyS0 "
    "console=ttyS0,115200 "
    "lpj=7999923 "
    "root=/dev/hda1 "
    "nokaslr "
)

# ---------------------------------------------------------------------------
# Checkpoint support
# ---------------------------------------------------------------------------
if args.restore_checkpoint:
    if not os.path.exists(args.restore_checkpoint):
        fatal("Checkpoint dir not found: %s" % args.restore_checkpoint)
    print("[vmx_research] Restoring from checkpoint: %s" % args.restore_checkpoint)
    m5.instantiate(args.restore_checkpoint)
else:
    m5.instantiate()

# ---------------------------------------------------------------------------
# VMX / EPT debug flags
# Uncomment the relevant DebugFlags once you have implemented the
# corresponding gem5 debug flag in your VMX source files.
# Pattern: add DPRINTF(VMX, "...") in your C++ code, declare the flag
# in src/arch/x86/SConscript, then enable it here.
# ---------------------------------------------------------------------------
if args.vmx_debug:
    # m5.trace.set_drain_event(...)  # placeholder
    print("[vmx_research] VMX debug output enabled")
    print("               Add --debug-flags=VMX to your gem5.opt invocation")

if args.ept_debug:
    print("[vmx_research] EPT debug output enabled")
    print("               Add --debug-flags=EPT to your gem5.opt invocation")

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
print("[vmx_research] Starting simulation")
print("  CPU type  : %s x%d" % (args.cpu_type, args.num_cpus))
print("  Memory    : %s %s" % (args.mem_size, args.mem_type))
print("  Kernel    : %s" % args.kernel)
print("  Disk      : %s" % args.disk_image)
print("  Caches    : L1=%s/%s L2=%s" % (
    args.l1i_size, args.l1d_size, args.l2_size) if args.caches else "  Caches    : disabled")
print("  Checkpoint: %s" % (args.restore_checkpoint or "none"))

exit_event = m5.simulate(args.max_tick if args.max_tick else m5.MaxTick)

print("\n[vmx_research] Simulation exit: %s @ tick %d" % (
    exit_event.getCause(), m5.curTick()
))

# Save checkpoint on clean exit so you can restore past the boot phase
# on subsequent VMX test runs without re-booting every time
if exit_event.getCause() == "checkpoint":
    checkpoint_path = os.path.join(
        args.checkpoint_dir,
        "cpt.%d" % m5.curTick()
    )
    print("[vmx_research] Saving checkpoint to: %s" % checkpoint_path)
    m5.checkpoint(checkpoint_path)

sys.exit(exit_event.getCode())
