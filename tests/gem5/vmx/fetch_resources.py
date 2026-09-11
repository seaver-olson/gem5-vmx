"""Pre-fetch the gem5 kernel/disk-image resources this test suite needs.

Run this once (or whenever the local resource cache is empty) before
building the kernel modules with `make -C tests/gem5/vmx`. The Makefile
needs the unstripped kernel binary already present on disk so it can read
the module-version CRCs the running kernel expects before any of the VMX
tests actually boot it; `config.py` would otherwise only fetch it lazily,
after the Makefile has already needed it.

Run with the gem5 binary itself, since the `gem5.resources` package lives
in gem5's embedded Python environment, not the system interpreter:

    build/X86/gem5.opt tests/gem5/vmx/fetch_resources.py
"""

import argparse
from pathlib import Path

from gem5.resources.resource import obtain_resource

TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[2]
DEFAULT_RESOURCE_DIR = REPO_ROOT / "resources"

# Kept in sync with config.py's board.set_kernel_disk_workload() call and
# with the Makefile's RESOURCE_KERNEL default.
RESOURCES = (
    ("x86-linux-kernel-5.4.49", "1.0.0"),
    ("x86-ubuntu-18.04-img", "1.0.0"),
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--resource-directory", type=Path, default=DEFAULT_RESOURCE_DIR
)
args = parser.parse_args()
args.resource_directory.mkdir(parents=True, exist_ok=True)

for resource_id, version in RESOURCES:
    print(f"fetch_resources: obtaining {resource_id}-{version} ...")
    obtain_resource(
        resource_id,
        resource_directory=str(args.resource_directory),
        resource_version=version,
    )
print("fetch_resources: done.")
