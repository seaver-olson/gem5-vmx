"""Full-system harness for the gem5 VMX smoke/checkpoint test."""

import argparse
import base64
import textwrap
from pathlib import Path

from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
    obtain_resource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires


TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[2]
DEFAULT_RESOURCE_DIR = REPO_ROOT / "resources"
DEFAULT_MODULE = TEST_DIR / "vmx_smoke.ko"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--module", type=Path, default=DEFAULT_MODULE)
parser.add_argument(
    "--resource-directory", type=Path, default=DEFAULT_RESOURCE_DIR
)
parser.add_argument(
    "--no-kvm",
    action="store_true",
    help="Boot directly on the Atomic CPU instead of KVM, which is slower.",
)
checkpoint_group = parser.add_mutually_exclusive_group()
checkpoint_group.add_argument("--take-checkpoint", type=Path)
checkpoint_group.add_argument("--restore", type=Path)
checkpoint_group.add_argument("--take-boot-checkpoint", type=Path)
checkpoint_group.add_argument("--restore-boot", type=Path)
args = parser.parse_args()
module_name = args.module.stem
guest_module_path = f"/root/{args.module.name}"

if not args.module.is_file():
    parser.error(f"module not found: {args.module}; run `make -C {TEST_DIR}`")
if args.restore and not (args.restore / "m5.cpt").is_file():
    parser.error(f"not a gem5 checkpoint directory: {args.restore}")
if args.restore_boot and not (args.restore_boot / "m5.cpt").is_file():
    parser.error(f"not a gem5 checkpoint directory: {args.restore_boot}")
if (args.take_boot_checkpoint or args.restore_boot) and not args.no_kvm:
    parser.error("boot checkpoints require --no-kvm")

requires(isa_required=ISA.X86, kvm_required=not args.no_kvm)


def embedded_file(path: str, contents: bytes) -> str:
    encoded = base64.b64encode(contents).decode("ascii")
    wrapped = "\n".join(textwrap.wrap(encoded, width=76))
    return (
        f"cat > {path}.b64 <<'__VMX_SMOKE_MODULE__'\n"
        f"{wrapped}\n"
        "__VMX_SMOKE_MODULE__\n"
        f"base64 -d {path}.b64 > {path}\n"
        f"rm -f {path}.b64\n"
    )


def guest_script(include_checkpoint: bool, switch_from_kvm: bool) -> str:
    checkpoint_command = ""
    if include_checkpoint:
        checkpoint_command = (
            'echo "vmx-smoke-harness: requesting checkpoint with VMX active"\n'
            "/sbin/m5 checkpoint\n"
            'echo "vmx-smoke-harness: resumed after checkpoint"\n'
        )

    switch_command = ""
    if switch_from_kvm:
        switch_command = (
            'echo "vmx-smoke-harness: switching from KVM to Atomic CPU"\n'
            # The supplied image's older m5 binary lacks a switchcpu command.
            # The first EXIT event is intercepted by exit_handler below.
            "/sbin/m5 exit\n"
            'echo "vmx-smoke-harness: running on Atomic CPU"\n'
        )

    return (
        "#!/bin/sh\n"
        "set -u\n"
        + embedded_file(guest_module_path, args.module.read_bytes())
        + switch_command
        + 'echo "vmx-smoke-harness: loading module"\n'
        f"if ! insmod {guest_module_path}; then\n"
        '    echo "vmx-smoke-harness: FAIL: insmod"\n'
        "    dmesg | tail -n 100\n"
        "    /sbin/m5 exit\n"
        "fi\n"
        + checkpoint_command
        + 'echo "vmx-smoke-harness: unloading module"\n'
        f"if ! rmmod {module_name}; then\n"
        f'    echo "{module_name}: FAIL: rmmod"\n'
        "    dmesg | tail -n 100\n"
        "    /sbin/m5 exit\n"
        "    exit 1\n"
        "fi\n"
        "dmesg | tail -n 100\n"
        'echo "vmx-smoke-harness: done"\n'
        "/sbin/m5 exit\n"
    )


if args.no_kvm:
    processor = SimpleProcessor(
        cpu_type=CPUTypes.ATOMIC,
        isa=ISA.X86,
        num_cores=1,
    )
else:
    processor = SimpleSwitchableProcessor(
        starting_core_type=CPUTypes.KVM,
        switch_core_type=CPUTypes.ATOMIC,
        isa=ISA.X86,
        num_cores=1,
    )
    if args.restore:
        # The checkpoint is taken after the KVM-to-Atomic switch.  gem5 only
        # serializes the active ("switch") CPU, so recreate that switched
        # state before unserialization; otherwise it looks for a nonexistent
        # board.processor.start.core section in m5.cpt.
        for core in processor.start:
            core.set_switched_out(True)
        switch_cores = processor._switchable_cores["switch"]
        for core in switch_cores:
            core.set_switched_out(False)
        processor._current_cores = switch_cores
        processor._current_is_start = False
    # Some hosts restrict perf_event_open even when /dev/kvm is usable.
    for core in processor.start:
        core.core.usePerf = False
board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=SingleChannelDDR3_1600(size="1GiB"),
    cache_hierarchy=PrivateL1CacheHierarchy(
        l1d_size="16KiB", l1i_size="16KiB"
    ),
)

kernel_path = args.resource_directory / "x86-linux-kernel-5.4.49-1.0.0"
disk_path = args.resource_directory / "x86-ubuntu-18.04-img-1.0.0"
workload = {
    "kernel": (
        KernelResource(local_path=str(kernel_path))
        if kernel_path.is_file()
        else obtain_resource(
            "x86-linux-kernel-5.4.49",
            resource_directory=str(args.resource_directory),
            resource_version="1.0.0",
        )
    ),
    "disk_image": (
        DiskImageResource(local_path=str(disk_path), root_partition="1")
        if disk_path.is_file()
        else obtain_resource(
            "x86-ubuntu-18.04-img",
            resource_directory=str(args.resource_directory),
            resource_version="1.0.0",
        )
    ),
    "readfile_contents": guest_script(
        include_checkpoint=bool(args.take_checkpoint or args.restore),
        switch_from_kvm=not args.no_kvm,
    ),
}
if args.take_boot_checkpoint:
    # Resume before reading the payload so a restored boot can test a newly
    # built module. No module or VMX state is captured in this checkpoint.
    workload["readfile_contents"] = (
        "#!/bin/sh\n"
        "/sbin/m5 checkpoint\n"
        "/sbin/m5 readfile > /root/vmx-run.sh\n"
        "exec /bin/sh /root/vmx-run.sh\n"
    )
if args.restore:
    workload["checkpoint"] = args.restore
elif args.restore_boot:
    workload["checkpoint"] = args.restore_boot
board.set_kernel_disk_workload(**workload)


def checkpoint_handler():
    destination = (args.take_boot_checkpoint or args.take_checkpoint).resolve()
    print(f"vmx-smoke-harness: saving checkpoint to {destination}")
    simulator.save_checkpoint(destination)
    print("vmx-smoke-harness: checkpoint saved")
    yield bool(args.take_boot_checkpoint)


def exit_handler():
    if not args.no_kvm and not args.restore:
        print("vmx-smoke-harness: switching processor to Atomic")
        simulator.switch_processor()
        yield False
    yield True


on_exit_event = {ExitEvent.EXIT: exit_handler()}
if args.take_checkpoint or args.take_boot_checkpoint:
    on_exit_event[ExitEvent.CHECKPOINT] = checkpoint_handler()

simulator = Simulator(board=board, on_exit_event=on_exit_event)
mode = (
    "boot-checkpoint" if args.take_boot_checkpoint
    else "restore-boot" if args.restore_boot
    else "restore" if args.restore
    else "checkpoint" if args.take_checkpoint
    else "normal"
)
print(f"vmx-smoke-harness: starting {mode} run")
simulator.run()
print(
    "vmx-smoke-harness: stopped at tick {} because {}".format(
        simulator.get_current_tick(), simulator.get_last_exit_event_cause()
    )
)
