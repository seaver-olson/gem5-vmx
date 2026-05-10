from pathlib import Path

from gem5.resources.resource import obtain_resource


REPO_ROOT = Path(__file__).resolve().parents[1]
RESOURCE_DIR = REPO_ROOT / "resources"


def fetch(resource_id, resource_version):
    resource = obtain_resource(
        resource_id,
        resource_directory=str(RESOURCE_DIR),
        resource_version=resource_version,
    )
    print(f"{resource_id}: {resource.get_local_path()}")


fetch("x86-linux-kernel-5.4.49", "1.0.0")
fetch("x86-ubuntu-18.04-img", "1.0.0")
