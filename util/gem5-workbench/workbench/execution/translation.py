"""Translate a project graph into the first supported gem5 run spec."""

from __future__ import annotations

import json
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from workbench.document import ProjectDocument
from workbench.execution.models import (
    BOARD_CACHE_PORT,
    BOARD_MEMORY_PORT,
    BOARD_PROCESSOR_PORT,
    BOARD_WORKLOAD_PORT,
    DDR3_MEMORY_TYPE_ID,
    NO_CACHE_TYPE_ID,
    SE_WORKLOAD_TYPE_ID,
    SIMPLE_BOARD_TYPE_ID,
    SIMPLE_PROCESSOR_TYPE_ID,
    SUPPORTED_COMPONENT_TYPE_IDS,
    ExecutionPlan,
    Gem5Installation,
    WorkloadKind,
)
from workbench.model import (
    Component,
    Endpoint,
)


class TranslationError(ValueError):
    """Raised when a project cannot be represented by the supported bridge."""


_ROLE_TYPES = {
    "board": SIMPLE_BOARD_TYPE_ID,
    "processor": SIMPLE_PROCESSOR_TYPE_ID,
    "memory": DDR3_MEMORY_TYPE_ID,
    "cache_hierarchy": NO_CACHE_TYPE_ID,
    "workload": SE_WORKLOAD_TYPE_ID,
}
_BOARD_PORT_ROLES = {
    BOARD_PROCESSOR_PORT: "processor",
    BOARD_MEMORY_PORT: "memory",
    BOARD_CACHE_PORT: "cache_hierarchy",
    BOARD_WORKLOAD_PORT: "workload",
}


def _only_component(components: list[Component], role: str) -> Component:
    if len(components) != 1:
        raise TranslationError(
            f"the x86 runner requires exactly one {role}; found {len(components)}"
        )
    return components[0]


def _parameters(
    component: Component, allowed: set[str]
) -> Mapping[str, object]:
    unknown = sorted(set(component.parameters) - allowed)
    if unknown:
        raise TranslationError(
            f"component {component.id} has unsupported parameter(s): "
            + ", ".join(unknown)
        )
    return component.parameters


def _text(value: object, field: str, default: str | None = None) -> str:
    if value is None and default is not None:
        value = default
    if not isinstance(value, str) or not value.strip():
        raise TranslationError(f"{field} must be a non-empty string")
    return value.strip()


def _positive_integer(value: object, field: str, default: int) -> int:
    if value is None:
        value = default
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise TranslationError(f"{field} must be a positive integer")
    if value > 1024:
        raise TranslationError(f"{field} must not exceed 1024")
    return value


def _component_entry(
    type_id: str, parameters: dict[str, Any]
) -> dict[str, Any]:
    return {"type_id": type_id, "parameters": parameters}


def _other_endpoint(
    board_id: str, source: Endpoint, target: Endpoint
) -> tuple[Endpoint, str] | None:
    if source.component_id == board_id:
        return target, source.port_id
    if target.component_id == board_id:
        return source, target.port_id
    return None


def _validate_connections(
    document: ProjectDocument,
    components: Mapping[str, Component],
) -> None:
    board = components["board"]
    bindings: dict[str, str] = {}
    for connection in document.project.connections.values():
        if connection.parameters:
            raise TranslationError(
                f"connection {connection.id} has unsupported parameters"
            )
        if (
            connection.source.slot is not None
            or connection.target.slot is not None
        ):
            raise TranslationError(
                f"connection {connection.id} uses a vector slot on a scalar port"
            )
        endpoints = _other_endpoint(
            str(board.id), connection.source, connection.target
        )
        if endpoints is None:
            raise TranslationError(
                f"connection {connection.id} does not bind a SimpleBoard input"
            )
        other, board_port = endpoints
        role = _BOARD_PORT_ROLES.get(board_port)
        if role is None:
            raise TranslationError(
                f"connection {connection.id} uses unsupported board port "
                f"{board_port!r}"
            )
        expected = components[role]
        if other.component_id != expected.id:
            raise TranslationError(
                f"board port {board_port!r} must connect to the {role} component"
            )
        if other.port_id != board_port:
            raise TranslationError(
                f"component {other.component_id} must use its output port; "
                f"found {other.port_id!r}"
            )
        if board_port in bindings:
            raise TranslationError(
                f"board port {board_port!r} is connected twice"
            )
        bindings[board_port] = str(other.component_id)

    missing = sorted(set(_BOARD_PORT_ROLES) - set(bindings))
    if missing:
        raise TranslationError(
            "SimpleBoard is missing required connection(s): "
            + ", ".join(missing)
        )


def translate_project(
    document: ProjectDocument,
    installation: Gem5Installation | None = None,
    *,
    project_directory: Path | None = None,
) -> ExecutionPlan:
    """Translate the explicit five-node x86 graph to a safe execution plan."""

    by_type: dict[str, list[Component]] = {
        type_id: [] for type_id in SUPPORTED_COMPONENT_TYPE_IDS
    }
    for component in document.project.components.values():
        if component.type_id not in by_type:
            raise TranslationError(
                f"component {component.id} has no executable adapter: "
                f"{component.type_id}"
            )
        by_type[component.type_id].append(component)

    components = {
        role: _only_component(by_type[type_id], role)
        for role, type_id in _ROLE_TYPES.items()
    }
    _validate_connections(document, components)

    board_parameters = _parameters(components["board"], {"clk_freq"})
    board = _component_entry(
        SIMPLE_BOARD_TYPE_ID,
        {
            "clk_freq": _text(
                board_parameters.get("clk_freq"), "board.clk_freq", "3GHz"
            )
        },
    )

    processor_parameters = _parameters(
        components["processor"], {"cpu_type", "num_cores", "isa"}
    )
    cpu_type = _text(
        processor_parameters.get("cpu_type"),
        "processor.cpu_type",
        "atomic",
    ).lower()
    if cpu_type not in {"atomic", "timing"}:
        raise TranslationError(
            "processor.cpu_type must be atomic or timing for x86"
        )
    isa = _text(
        processor_parameters.get("isa"), "processor.isa", "x86"
    ).lower()
    if isa != "x86":
        raise TranslationError("the first execution adapter requires ISA X86")
    processor = _component_entry(
        SIMPLE_PROCESSOR_TYPE_ID,
        {
            "cpu_type": cpu_type,
            "num_cores": _positive_integer(
                processor_parameters.get("num_cores"),
                "processor.num_cores",
                1,
            ),
            "isa": isa,
        },
    )

    memory_parameters = _parameters(components["memory"], {"size"})
    memory = _component_entry(
        DDR3_MEMORY_TYPE_ID,
        {"size": _text(memory_parameters.get("size"), "memory.size", "32MiB")},
    )

    _parameters(components["cache_hierarchy"], set())
    cache = _component_entry(NO_CACHE_TYPE_ID, {})

    workload_parameters = _parameters(
        components["workload"],
        {
            "source_kind",
            "local_path",
            "path_base",
            "resource_id",
            "resource_version",
        },
    )
    kind_text = _text(
        workload_parameters.get("source_kind"),
        "workload.source_kind",
        WorkloadKind.LOCAL.value,
    ).lower()
    try:
        kind = WorkloadKind(kind_text)
    except ValueError as error:
        raise TranslationError(
            "workload.kind must be local or resource"
        ) from error

    translated_workload: dict[str, Any] = {
        "kind": kind.value,
        "arguments": [],
    }
    if kind is WorkloadKind.LOCAL:
        raw_path = workload_parameters.get("local_path")
        if raw_path is None:
            raw_path = "tests/test-progs/hello/bin/x86/linux/hello"
        path = Path(_text(raw_path, "workload.local_path")).expanduser()
        path_base = _text(
            workload_parameters.get("path_base"),
            "workload.path_base",
            "gem5_root",
        ).lower()
        if path_base == "gem5_root":
            if installation is None:
                raise TranslationError(
                    "path_base gem5_root requires a gem5 installation"
                )
            if not path.is_absolute():
                path = installation.repo_root / path
        elif path_base == "project":
            if project_directory is None:
                raise TranslationError(
                    "path_base project requires the saved project path"
                )
            if not path.is_absolute():
                path = project_directory.resolve() / path
        elif path_base == "absolute":
            if not path.is_absolute():
                raise TranslationError(
                    "path_base absolute requires an absolute local_path"
                )
        else:
            raise TranslationError(
                "workload.path_base must be gem5_root, project, or absolute"
            )
        translated_workload["path"] = str(path.resolve())
    else:
        translated_workload["resource_id"] = _text(
            workload_parameters.get("resource_id"),
            "workload.resource_id",
            "x86-hello64-static",
        )
        version = workload_parameters.get("resource_version", "1.0.0")
        if version is not None:
            translated_workload["resource_version"] = _text(
                version, "workload.resource_version"
            )

    workload = _component_entry(SE_WORKLOAD_TYPE_ID, translated_workload)
    component_ids = {
        role: str(component.id) for role, component in components.items()
    }
    return ExecutionPlan(
        board=board,
        processor=processor,
        memory=memory,
        cache_hierarchy=cache,
        workload=workload,
        component_ids=component_ids,
    )


def dumps_execution_plan(plan: ExecutionPlan) -> str:
    return (
        json.dumps(plan.to_data(), indent=2, sort_keys=True, allow_nan=False)
        + "\n"
    )
