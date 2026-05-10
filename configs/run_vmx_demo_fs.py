import argparse
import base64
import textwrap
from pathlib import Path

from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.memory import DualChannelDDR3_1600
from gem5.components.processors.cpu_types import (
    get_cpu_type_from_str,
    get_cpu_types_str_set,
)
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RESOURCE_DIR = REPO_ROOT / "resources"
DEMO_DIR = REPO_ROOT / "demos/vmx_shadow_vmcs"
DEMO_SOURCE = DEMO_DIR / "vmx_shadow_vmcs_demo.c"
DEMO_MAKEFILE = DEMO_DIR / "Makefile"
DEFAULT_MODULE = DEMO_DIR / "vmx_shadow_vmcs_demo.ko"


parser = argparse.ArgumentParser(
    description="Boot the x86 Linux kernel/disk pair used by the VMX demo."
)
parser.add_argument(
    "--resource-directory",
    default=str(DEFAULT_RESOURCE_DIR),
    help="Directory where gem5 resources are downloaded or already present.",
)
parser.add_argument(
    "--cpu",
    default="timing",
    choices=get_cpu_types_str_set(),
    help="CPU model used for the full-system run.",
)
parser.add_argument(
    "--num-cpus",
    type=int,
    default=1,
    help="Number of simulated x86 CPUs.",
)
parser.add_argument(
    "--readfile",
    default=None,
    help=(
        "Optional guest script consumed through m5 readfile. If omitted, this "
        "config generates a self-contained script that builds and loads the "
        "VMX demo module inside the guest."
    ),
)
parser.add_argument(
    "--module",
    default=str(DEFAULT_MODULE),
    help=(
        "Optional prebuilt vmx_shadow_vmcs_demo.ko to embed in the guest "
        "readfile. If the file is missing, the generated readfile falls back "
        "to an in-guest build attempt."
    ),
)
parser.add_argument(
    "--tick-exit",
    type=int,
    default=None,
    help="Optional max tick limit for smoke testing.",
)

args = parser.parse_args()


def guest_file_write_command(path, source_path, marker):
    return (
        f"cat > {path} <<'{marker}'\n"
        f"{source_path.read_text()}"
        f"\n{marker}\n"
    )


def guest_base64_write_command(path, source_path, marker):
    encoded = base64.b64encode(source_path.read_bytes()).decode("ascii")
    wrapped = "\n".join(textwrap.wrap(encoded, width=76))
    return (
        f"cat > {path}.b64 <<'{marker}'\n"
        f"{wrapped}\n"
        f"{marker}\n"
        f"base64 -d {path}.b64 > {path}\n"
        f"rm -f {path}.b64\n"
    )


def make_prebuilt_module_readfile_contents(module_path):
    return (
        "#!/bin/sh\n"
        "set -eu\n"
        "\n"
        "echo \"vmx-demo: installing embedded prebuilt module\"\n"
        + guest_base64_write_command(
            "/root/vmx_shadow_vmcs_demo.ko",
            module_path,
            "__VMX_DEMO_MODULE__",
        )
        + "chmod 0600 /root/vmx_shadow_vmcs_demo.ko\n"
        "\n"
        "echo \"vmx-demo: loading /root/vmx_shadow_vmcs_demo.ko\"\n"
        "insmod /root/vmx_shadow_vmcs_demo.ko || true\n"
        "\n"
        "echo \"vmx-demo: recent kernel log\"\n"
        "dmesg | tail -n 60\n"
        "\n"
        "echo \"vmx-demo: unloading vmx_shadow_vmcs_demo\"\n"
        "rmmod vmx_shadow_vmcs_demo || true\n"
        "\n"
        "echo \"vmx-demo: exiting gem5\"\n"
        "/sbin/m5 exit\n"
    )


def make_default_readfile_contents():
    module_dir = "/root/vmx_shadow_vmcs_demo"
    return (
        "#!/bin/sh\n"
        "set -eu\n"
        "\n"
        "echo \"vmx-demo: preparing in-guest module build\"\n"
        f"mkdir -p {module_dir}\n"
        f"cd {module_dir}\n"
        "\n"
        + guest_file_write_command(
            "Makefile", DEMO_MAKEFILE, "__VMX_DEMO_MAKEFILE__"
        )
        + guest_file_write_command(
            "vmx_shadow_vmcs_demo.c", DEMO_SOURCE, "__VMX_DEMO_SOURCE__"
        )
        + "\n"
        "KREL=$(uname -r)\n"
        "KBUILD=/lib/modules/${KREL}/build\n"
        "echo \"vmx-demo: kernel release ${KREL}\"\n"
        "if [ ! -d \"${KBUILD}\" ]; then\n"
        "    echo \"vmx-demo: ERROR: missing ${KBUILD}; cannot build kernel module\"\n"
        "    echo \"vmx-demo: install/provide the matching kernel build tree or prebuild vmx_shadow_vmcs_demo.ko\"\n"
        "    dmesg | tail -n 40\n"
        "    /sbin/m5 exit\n"
        "fi\n"
        "\n"
        "echo \"vmx-demo: building vmx_shadow_vmcs_demo.ko\"\n"
        "make\n"
        "\n"
        "echo \"vmx-demo: loading ${PWD}/vmx_shadow_vmcs_demo.ko\"\n"
        "insmod ./vmx_shadow_vmcs_demo.ko || true\n"
        "\n"
        "echo \"vmx-demo: recent kernel log\"\n"
        "dmesg | tail -n 60\n"
        "\n"
        "echo \"vmx-demo: unloading vmx_shadow_vmcs_demo\"\n"
        "rmmod vmx_shadow_vmcs_demo || true\n"
        "\n"
        "echo \"vmx-demo: exiting gem5\"\n"
        "/sbin/m5 exit\n"
    )

requires(isa_required=ISA.X86, kvm_required=(args.cpu == "kvm"))

cache_hierarchy = PrivateL1CacheHierarchy(l1d_size="16KiB", l1i_size="16KiB")
memory = DualChannelDDR3_1600(size="3GiB")
processor = SimpleProcessor(
    cpu_type=get_cpu_type_from_str(args.cpu),
    isa=ISA.X86,
    num_cores=args.num_cpus,
)

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

workload_args = {
    "kernel": obtain_resource(
        "x86-linux-kernel-5.4.49",
        resource_directory=args.resource_directory,
        resource_version="1.0.0",
    ),
    "disk_image": obtain_resource(
        "x86-ubuntu-18.04-img",
        resource_directory=args.resource_directory,
        resource_version="1.0.0",
    ),
}

if args.readfile:
    workload_args["readfile"] = args.readfile
else:
    module_path = Path(args.module)
    if module_path.exists():
        workload_args["readfile_contents"] = make_prebuilt_module_readfile_contents(
            module_path
        )
    else:
        workload_args["readfile_contents"] = make_default_readfile_contents()

board.set_kernel_disk_workload(**workload_args)

simulator_args = {"board": board}
if args.tick_exit is not None:
    simulator_args["max_ticks"] = args.tick_exit

simulator = Simulator(**simulator_args)

print("vmx-demo: beginning x86 full-system simulation")
print(f"vmx-demo: resources: {args.resource_directory}")
if args.readfile:
    print(f"vmx-demo: readfile: {args.readfile}")
else:
    module_path = Path(args.module)
    if module_path.exists():
        print(f"vmx-demo: readfile: embedding prebuilt module {module_path}")
    else:
        print(
            "vmx-demo: readfile: generated in-guest module build script "
            f"because {module_path} is missing"
        )

simulator.run()

print(
    "vmx-demo: exiting @ tick {} because {}.".format(
        simulator.get_current_tick(), simulator.get_last_exit_event_cause()
    )
)
