"""Reproducible job artifact and manifest creation."""

from __future__ import annotations

import json
import os
import re
import shutil
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path
from typing import Any
from uuid import uuid4

from workbench.document import ProjectDocument
from workbench.execution.installation import (
    fingerprint_file,
    installation_fingerprint,
)
from workbench.execution.models import (
    ExecutionPlan,
    Gem5Installation,
    JobKind,
    JobState,
)
from workbench.execution.translation import dumps_execution_plan
from workbench.persistence import dumps_project_document


@dataclass(frozen=True, slots=True)
class ArtifactBundle:
    job_id: str
    directory: Path
    log_path: Path
    manifest_path: Path
    spec_path: Path | None = None
    snapshot_path: Path | None = None
    bridge_path: Path | None = None


def _timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _safe_name(value: str) -> str:
    normalized = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip())
    normalized = normalized.strip(".-")
    return normalized[:80] or "project"


def _job_id() -> str:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return f"{timestamp}-{uuid4().hex[:8]}"


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(f".{path.name}.{uuid4().hex}.tmp")
    try:
        temporary.write_text(
            json.dumps(value, indent=2, sort_keys=True, allow_nan=False)
            + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _installation_data(installation: Gem5Installation) -> dict[str, Any]:
    fingerprint = installation_fingerprint(installation)
    return {
        "repo_root": str(installation.repo_root),
        "binary": str(installation.binary) if installation.binary else None,
        "build_target": installation.build_target,
        "source_revision": installation.source_revision,
        "fingerprint": fingerprint,
        "binary_sha256": (
            fingerprint_file(installation.binary)
            if installation.binary is not None
            and installation.binary.is_file()
            else None
        ),
    }


_REPRODUCIBILITY_ENVIRONMENT = (
    "GEM5_CONFIG",
    "GEM5_RESOURCE_DIR",
    "GEM5_RESOURCE_JSON",
    "GEM5_RESOURCE_JSON_APPEND",
    "GEM5_USE_PROXY",
    "LD_LIBRARY_PATH",
    "M5_PATH",
    "PATH",
    "PYTHONPATH",
)


def _environment_data(
    environment: Mapping[str, str] | None,
) -> dict[str, str]:
    source = os.environ if environment is None else environment
    return {
        name: source[name]
        for name in _REPRODUCIBILITY_ENVIRONMENT
        if name in source
    }


def _workload_data(plan: ExecutionPlan) -> dict[str, Any]:
    parameters = plan.workload["parameters"]
    kind = str(parameters["kind"])
    if kind == "local":
        path = Path(str(parameters["path"]))
        return {
            "kind": kind,
            "path": str(path),
            "size": path.stat().st_size,
            "sha256": fingerprint_file(path),
        }
    return {
        "kind": kind,
        "resource_id": parameters["resource_id"],
        "resource_version": parameters.get("resource_version"),
    }


def _snapshot_local_workload(
    plan: ExecutionPlan, directory: Path
) -> tuple[ExecutionPlan, Path | None, Path | None]:
    parameters = plan.workload["parameters"]
    if parameters["kind"] != "local":
        return plan, None, None

    source = Path(str(parameters["path"]))
    snapshot = directory / "workload.bin"
    shutil.copyfile(source, snapshot)
    snapshot_parameters = dict(parameters)
    snapshot_parameters["path"] = str(snapshot)
    workload = dict(plan.workload)
    workload["parameters"] = snapshot_parameters
    return (
        ExecutionPlan(
            board=plan.board,
            processor=plan.processor,
            memory=plan.memory,
            cache_hierarchy=plan.cache_hierarchy,
            workload=workload,
            component_ids=plan.component_ids,
            isa=plan.isa,
        ),
        source,
        snapshot,
    )


def _artifact(path: str, directory: Path) -> dict[str, Any]:
    return {"path": path, "exists": (directory / path).exists()}


def prepare_run_artifacts(
    output_root: Path,
    project_name: str,
    document: ProjectDocument,
    plan: ExecutionPlan,
    installation: Gem5Installation,
    catalog_fingerprint: str | None = None,
    *,
    bridge_source: Path,
    environment: Mapping[str, str] | None = None,
) -> ArtifactBundle:
    """Create a run snapshot using the supplied gem5 source fingerprint."""

    job_id = _job_id()
    directory = output_root / _safe_name(project_name) / job_id
    spec_path = directory / "run-spec.json"
    snapshot_path = directory / "project.g5proj"
    bridge_path = directory / "bridge.py"
    log_path = directory / "gem5.log"
    manifest_path = directory / "manifest.json"
    created = False
    try:
        directory.mkdir(parents=True, exist_ok=False)
        created = True
        effective_plan, workload_source, workload_snapshot = (
            _snapshot_local_workload(plan, directory)
        )
        spec_path.write_text(
            dumps_execution_plan(effective_plan), encoding="utf-8"
        )
        snapshot_path.write_text(
            dumps_project_document(document), encoding="utf-8"
        )
        shutil.copyfile(bridge_source, bridge_path)
        artifacts = {
            name: _artifact(path, directory)
            for name, path in {
                "project": snapshot_path.name,
                "run_spec": spec_path.name,
                "bridge": bridge_path.name,
                "log": log_path.name,
                "stats": "stats.txt",
                "config_ini": "config.ini",
                "config_json": "config.json",
                "command": "command.json",
                "status": "status.json",
            }.items()
        }
        if workload_snapshot is not None:
            artifacts["workload"] = _artifact(
                workload_snapshot.name, directory
            )
        workload_data = _workload_data(effective_plan)
        if workload_source is not None:
            workload_data["source_path"] = str(workload_source)
        write_json_atomic(
            manifest_path,
            {
                "format": "gem5-workbench-run-manifest",
                "schema_version": 2,
                "job_id": job_id,
                "kind": JobKind.RUN.value,
                "status": JobState.STARTING.value,
                "created_at": _timestamp(),
                "updated_at": _timestamp(),
                "installation": _installation_data(installation),
                "catalog_fingerprint": catalog_fingerprint,
                "execution_context": {
                    "cwd": str(installation.repo_root),
                    "environment": _environment_data(environment),
                    "bridge": {
                        "source_path": str(bridge_source),
                        "sha256": fingerprint_file(bridge_path),
                    },
                    "workload": workload_data,
                },
                "artifacts": artifacts,
                "command": None,
                "return_code": None,
                "error": None,
            },
        )
    except Exception:
        if created:
            shutil.rmtree(directory, ignore_errors=True)
        raise
    return ArtifactBundle(
        job_id=job_id,
        directory=directory,
        log_path=log_path,
        manifest_path=manifest_path,
        spec_path=spec_path,
        snapshot_path=snapshot_path,
        bridge_path=bridge_path,
    )


def prepare_build_artifacts(
    output_root: Path,
    project_name: str,
    installation: Gem5Installation,
    *,
    environment: Mapping[str, str] | None = None,
) -> ArtifactBundle:
    job_id = _job_id()
    directory = output_root / "builds" / _safe_name(project_name) / job_id
    log_path = directory / "build.log"
    manifest_path = directory / "manifest.json"
    created = False
    try:
        directory.mkdir(parents=True, exist_ok=False)
        created = True
        artifacts = {
            name: _artifact(path, directory)
            for name, path in {
                "log": log_path.name,
                "command": "command.json",
                "status": "status.json",
            }.items()
        }
        write_json_atomic(
            manifest_path,
            {
                "format": "gem5-workbench-build-manifest",
                "schema_version": 2,
                "job_id": job_id,
                "kind": JobKind.BUILD.value,
                "status": JobState.STARTING.value,
                "created_at": _timestamp(),
                "updated_at": _timestamp(),
                "installation": _installation_data(installation),
                "execution_context": {
                    "cwd": str(installation.repo_root),
                    "environment": _environment_data(environment),
                },
                "artifacts": artifacts,
                "command": None,
                "return_code": None,
                "error": None,
            },
        )
    except Exception:
        if created:
            shutil.rmtree(directory, ignore_errors=True)
        raise
    return ArtifactBundle(job_id, directory, log_path, manifest_path)


def update_manifest(
    bundle: ArtifactBundle,
    *,
    state: JobState,
    command: list[str] | None = None,
    return_code: int | None = None,
    error: str | None = None,
) -> None:
    try:
        data = json.loads(bundle.manifest_path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("manifest root must be an object")
    except (OSError, ValueError, RecursionError):
        data = {
            "format": "gem5-workbench-job-manifest",
            "schema_version": 2,
            "job_id": bundle.job_id,
        }
    try:
        previous_state = JobState(data.get("status"))
    except (TypeError, ValueError):
        previous_state = None
    if previous_state is not None:
        if previous_state.terminal and not state.terminal:
            return
        if previous_state is JobState.STOPPING and state in {
            JobState.STARTING,
            JobState.BUILDING,
            JobState.RUNNING,
        }:
            return
    data["status"] = state.value
    data["updated_at"] = _timestamp()
    if command is not None:
        data["command"] = command
    data["return_code"] = return_code
    data["error"] = error
    if command is not None:
        write_json_atomic(
            bundle.directory / "command.json", {"argv": list(command)}
        )
    write_json_atomic(
        bundle.directory / "status.json",
        {
            "job_id": bundle.job_id,
            "status": state.value,
            "return_code": return_code,
            "error": error,
            "updated_at": data["updated_at"],
        },
    )
    artifacts = data.get("artifacts")
    if isinstance(artifacts, dict):
        for value in artifacts.values():
            if isinstance(value, dict) and isinstance(value.get("path"), str):
                value["exists"] = (bundle.directory / value["path"]).exists()
    write_json_atomic(bundle.manifest_path, data)
