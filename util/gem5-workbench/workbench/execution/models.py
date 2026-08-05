"""Rendering-independent models used to build and run gem5 jobs."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import (
    TYPE_CHECKING,
    Any,
    Mapping,
)

from workbench.model.values import freeze_json_value
from workbench.registry.builtins import (
    DDR3_MEMORY_TYPE_ID,
    NO_CACHE_TYPE_ID,
    SE_WORKLOAD_TYPE_ID,
    SIMPLE_BOARD_TYPE_ID,
    SIMPLE_PROCESSOR_TYPE_ID,
)

if TYPE_CHECKING:
    from workbench.document import ProjectDocument


def _freeze(value: Any, field: str) -> Any:
    """Validate and freeze JSON data without coercing ambiguous keys."""

    return freeze_json_value(value, field)


def _thaw(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _thaw(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw(item) for item in value]
    return value


RUN_SPEC_FORMAT = "gem5-workbench-run"
RUN_SPEC_VERSION = 1

SUPPORTED_COMPONENT_TYPE_IDS = frozenset(
    {
        SIMPLE_BOARD_TYPE_ID,
        SIMPLE_PROCESSOR_TYPE_ID,
        DDR3_MEMORY_TYPE_ID,
        NO_CACHE_TYPE_ID,
        SE_WORKLOAD_TYPE_ID,
    }
)

BOARD_PROCESSOR_PORT = "processor"
BOARD_MEMORY_PORT = "memory"
BOARD_CACHE_PORT = "cache_hierarchy"
BOARD_WORKLOAD_PORT = "workload"


class WorkloadKind(Enum):
    LOCAL = "local"
    RESOURCE = "resource"


class JobKind(Enum):
    BUILD = "build"
    RUN = "run"


class JobState(Enum):
    IDLE = "idle"
    STARTING = "starting"
    BUILDING = "building"
    RUNNING = "running"
    STOPPING = "stopping"
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    CANCELLED = "cancelled"

    @property
    def terminal(self) -> bool:
        return self in {
            JobState.SUCCEEDED,
            JobState.FAILED,
            JobState.CANCELLED,
        }


@dataclass(frozen=True, slots=True)
class Gem5Installation:
    """A local gem5 source tree and an optional executable built from it."""

    repo_root: Path
    binary: Path | None = None
    build_target: str = "build/X86/gem5.opt"
    source_revision: str | None = None

    def __post_init__(self) -> None:
        if (
            not isinstance(self.build_target, str)
            or not self.build_target
            or self.build_target != self.build_target.strip()
            or "\x00" in self.build_target
        ):
            raise ValueError("gem5 build target must be a non-empty path")
        target = Path(self.build_target)
        if (
            target.is_absolute()
            or not target.parts
            or ".." in target.parts
            or self.build_target.startswith("-")
        ):
            raise ValueError(
                "gem5 build target must be a safe repository-relative path"
            )
        object.__setattr__(self, "repo_root", self.repo_root.resolve())
        if self.binary is not None:
            object.__setattr__(self, "binary", self.binary.resolve())

    @property
    def build_output(self) -> Path:
        return self.repo_root / self.build_target


@dataclass(frozen=True, slots=True)
class Gem5Probe:
    version: str
    supported_isas: tuple[str, ...]
    supported_protocols: tuple[str, ...]
    importable_type_ids: tuple[str, ...]
    binary_fingerprint: str

    def __post_init__(self) -> None:
        if (
            not isinstance(self.version, str)
            or not self.version
            or self.version != self.version.strip()
        ):
            raise ValueError("gem5 probe version must be a non-empty string")
        if (
            not isinstance(self.binary_fingerprint, str)
            or not self.binary_fingerprint
        ):
            raise ValueError(
                "gem5 probe binary fingerprint must be a non-empty string"
            )
        for field_name in (
            "supported_isas",
            "supported_protocols",
            "importable_type_ids",
        ):
            raw_values = getattr(self, field_name)
            if isinstance(raw_values, (str, bytes)):
                raise ValueError(
                    f"gem5 probe {field_name} must be a sequence of strings"
                )
            try:
                values = tuple(raw_values)
            except TypeError as error:
                raise ValueError(
                    f"gem5 probe {field_name} must be a sequence of strings"
                ) from error
            if not all(
                isinstance(value, str) and value and value == value.strip()
                for value in values
            ):
                raise ValueError(
                    f"gem5 probe {field_name} must contain non-empty strings"
                )
            object.__setattr__(self, field_name, values)

    @property
    def supports_x86(self) -> bool:
        return "x86" in self.supported_isas


@dataclass(frozen=True, slots=True)
class ExecutionPlan:
    """A validated, deterministic description consumed by the bridge."""

    board: Mapping[str, Any]
    processor: Mapping[str, Any]
    memory: Mapping[str, Any]
    cache_hierarchy: Mapping[str, Any]
    workload: Mapping[str, Any]
    component_ids: Mapping[str, str]
    isa: str = "x86"

    def __post_init__(self) -> None:
        for field_name in (
            "board",
            "processor",
            "memory",
            "cache_hierarchy",
            "workload",
            "component_ids",
        ):
            value = getattr(self, field_name)
            if not isinstance(value, Mapping):
                raise ValueError(
                    f"execution plan {field_name} must be an object"
                )
            object.__setattr__(
                self,
                field_name,
                _freeze(value, f"execution plan {field_name}"),
            )

    def to_data(self) -> dict[str, Any]:
        return {
            "format": RUN_SPEC_FORMAT,
            "schema_version": RUN_SPEC_VERSION,
            "isa": self.isa,
            "components": {
                "board": _thaw(self.board),
                "processor": _thaw(self.processor),
                "memory": _thaw(self.memory),
                "cache_hierarchy": _thaw(self.cache_hierarchy),
                "workload": _thaw(self.workload),
            },
            "component_ids": _thaw(self.component_ids),
        }


@dataclass(frozen=True, slots=True)
class RunHandle:
    job_id: str
    kind: JobKind
    output_dir: Path


@dataclass(frozen=True, slots=True)
class RunRequest:
    """Inputs for one immutable run artifact set.

    ``catalog_fingerprint`` identifies the gem5 Python source catalog used to
    validate the project. Binary identity is tracked separately by ``probe``
    and the installation section of the run manifest.
    """

    document: ProjectDocument
    installation: Gem5Installation
    project_name: str = "project"
    output_root: Path | None = None
    probe: Gem5Probe | None = None
    project_path: Path | None = None
    catalog_fingerprint: str | None = None


@dataclass(frozen=True, slots=True)
class BuildRequest:
    installation: Gem5Installation
    project_name: str = "gem5"
    output_root: Path | None = None
    jobs: int | None = None


@dataclass(frozen=True, slots=True)
class JobSnapshot:
    job_id: str | None
    kind: JobKind | None
    state: JobState
    output_dir: Path | None = None
    return_code: int | None = None
    error: str | None = None
    recent_output: str = ""
