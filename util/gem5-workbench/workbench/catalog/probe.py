"""Out-of-process runtime verification for source-discovered symbols."""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import signal
import subprocess
import tempfile
import threading
import time
from dataclasses import replace
from pathlib import Path
from typing import Mapping

from workbench.catalog.models import (
    Catalog,
    CatalogDiagnostic,
    CatalogEntry,
    DiagnosticSeverity,
    ParameterKind,
    ParameterSpec,
    RuntimeParameter,
    RuntimeProbeResult,
    RuntimeSymbol,
    SupportStatus,
    SymbolKind,
)


class RuntimeProbeError(RuntimeError):
    pass


_OUTPUT_TAIL_BYTES = 64 * 1024
_MAX_RESULT_BYTES = 8 * 1024 * 1024


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON object key: {key}")
        value[key] = item
    return value


def _read_result_json(path: Path) -> object:
    with path.open("rb") as stream:
        raw_value = stream.read(_MAX_RESULT_BYTES + 1)
    if len(raw_value) > _MAX_RESULT_BYTES:
        raise ValueError(
            f"runtime probe result exceeds {_MAX_RESULT_BYTES} bytes"
        )
    return json.loads(
        raw_value.decode("utf-8"), object_pairs_hook=_unique_json_object
    )


def _decode_tail(stream, limit: int = _OUTPUT_TAIL_BYTES) -> str:
    try:
        stream.flush()
        stream.seek(0, os.SEEK_END)
        size = stream.tell()
        stream.seek(max(0, size - limit))
        value = stream.read()
    except (OSError, ValueError):
        return ""
    return value.decode("utf-8", errors="replace")


def fingerprint_file(path: str | Path) -> str:
    """Return a content fingerprint for a gem5 executable or other file."""

    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _signal_probe(process: subprocess.Popen, requested_signal: int) -> None:
    try:
        pid = getattr(process, "pid", None)
        if os.name == "posix" and isinstance(pid, int) and pid > 0:
            os.killpg(pid, requested_signal)
        elif requested_signal == signal.SIGKILL:
            process.kill()
        elif requested_signal == signal.SIGTERM:
            process.terminate()
        else:
            process.send_signal(requested_signal)
    except (AttributeError, OSError, ProcessLookupError):
        pass


def _probe_group_alive(process: subprocess.Popen) -> bool:
    pid = getattr(process, "pid", None)
    if os.name != "posix" or not isinstance(pid, int) or pid <= 0:
        return False
    try:
        os.killpg(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def _stop_probe(process: subprocess.Popen) -> None:
    _signal_probe(process, signal.SIGTERM)
    try:
        process.wait(timeout=1)
    except (OSError, subprocess.TimeoutExpired):
        pass
    else:
        if not _probe_group_alive(process):
            return
    _signal_probe(process, signal.SIGKILL)
    try:
        process.wait(timeout=1)
    except (OSError, subprocess.TimeoutExpired):
        pass


def _run_probe_command(
    command: list[str],
    *,
    cwd: str | Path,
    environment: Mapping[str, str],
    timeout_s: float,
    cancel_event: threading.Event | None,
) -> subprocess.CompletedProcess[str]:
    # File-backed capture prevents a noisy gem5 process from accumulating an
    # unbounded stdout/stderr value in the Workbench process. Only the bounded
    # tails are decoded, with replacement for hostile byte sequences.
    try:
        stdout_stream = tempfile.TemporaryFile()
        try:
            stderr_stream = tempfile.TemporaryFile()
        except OSError:
            stdout_stream.close()
            raise
    except OSError as error:
        raise RuntimeProbeError(
            f"could not create catalog probe output files: {error}"
        ) from error
    with stdout_stream, stderr_stream:
        try:
            process = subprocess.Popen(
                command,
                cwd=cwd,
                env=environment,
                stdout=stdout_stream,
                stderr=stderr_stream,
                text=False,
                start_new_session=os.name == "posix",
            )
        except (OSError, TypeError, ValueError) as error:
            raise RuntimeProbeError(
                f"could not start gem5 catalog probe: {error}"
            ) from error
        deadline = time.monotonic() + timeout_s
        stop_attempted = False
        try:
            while True:
                if cancel_event is not None and cancel_event.is_set():
                    stop_attempted = True
                    _stop_probe(process)
                    raise RuntimeProbeError("gem5 catalog probe was cancelled")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    stop_attempted = True
                    _stop_probe(process)
                    raise RuntimeProbeError(
                        "gem5 catalog probe timed out after "
                        f"{timeout_s:g} seconds"
                    )
                try:
                    process.wait(timeout=min(0.1, remaining))
                    break
                except subprocess.TimeoutExpired:
                    continue
                except OSError as error:
                    stop_attempted = True
                    _stop_probe(process)
                    raise RuntimeProbeError(
                        f"could not wait for gem5 catalog probe: {error}"
                    ) from error
        except BaseException:
            if not stop_attempted and (
                process.returncode is None or _probe_group_alive(process)
            ):
                _stop_probe(process)
            raise
        return subprocess.CompletedProcess(
            command,
            process.returncode,
            _decode_tail(stdout_stream),
            _decode_tail(stderr_stream),
        )


def run_runtime_probe(
    gem5_binary: str | Path,
    catalog: Catalog,
    *,
    timeout_s: float = 60.0,
    environment: Mapping[str, str] | None = None,
    binary_fingerprint: str | None = None,
    cancel_event: threading.Event | None = None,
) -> RuntimeProbeResult:
    """Run the fixed probe script with gem5 and read its JSON result.

    Only symbols already discovered from the trusted local gem5 source tree
    are supplied to the child. The Workbench process never imports gem5.
    """

    binary = Path(gem5_binary).expanduser().resolve()
    if not binary.is_file():
        raise RuntimeProbeError(f"gem5 binary does not exist: {binary}")
    if (
        isinstance(timeout_s, bool)
        or not isinstance(timeout_s, (int, float))
        or not math.isfinite(timeout_s)
        or timeout_s <= 0
    ):
        raise ValueError("timeout_s must be positive")
    if cancel_event is not None and cancel_event.is_set():
        raise RuntimeProbeError("gem5 catalog probe was cancelled")
    if binary_fingerprint is None:
        binary_fingerprint = fingerprint_file(binary)
    elif not isinstance(binary_fingerprint, str) or not binary_fingerprint:
        raise ValueError("binary_fingerprint must be a non-empty string")
    probe_script = Path(__file__).with_name("runtime_probe.py")
    request = {
        "schema_version": 1,
        "source_fingerprint": catalog.source_fingerprint,
        "symbols": [
            {
                "type_id": entry.type_id,
                "module": entry.symbol.module,
                "qualified_name": entry.symbol.qualified_name,
                "symbol_kind": entry.symbol_kind.value,
            }
            for entry in catalog.entries
        ],
    }
    child_environment = os.environ.copy()
    if environment is not None:
        child_environment.update(environment)

    try:
        temporary_directory = tempfile.TemporaryDirectory(
            prefix="gem5-workbench-probe-"
        )
    except OSError as error:
        raise RuntimeProbeError(
            f"could not create catalog probe workspace: {error}"
        ) from error
    with temporary_directory as raw_dir:
        workspace = Path(raw_dir)
        request_path = workspace / "request.json"
        result_path = workspace / "result.json"
        try:
            request_path.write_text(
                json.dumps(request, sort_keys=True), encoding="utf-8"
            )
        except (OSError, TypeError, ValueError) as error:
            raise RuntimeProbeError(
                f"could not write catalog probe request: {error}"
            ) from error
        command = [
            str(binary),
            "-q",
            "--listener-mode=off",
            "-d",
            str(workspace / "m5out"),
            str(probe_script),
            "--catalog-input",
            str(request_path),
            "--catalog-output",
            str(result_path),
        ]
        completed = _run_probe_command(
            command,
            cwd=catalog.source_root,
            environment=child_environment,
            timeout_s=timeout_s,
            cancel_event=cancel_event,
        )

        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            if len(detail) > 2000:
                detail = detail[-2000:]
            raise RuntimeProbeError(
                f"gem5 catalog probe exited with {completed.returncode}"
                + (f": {detail}" if detail else "")
            )
        if not result_path.is_file():
            raise RuntimeProbeError(
                "gem5 catalog probe produced no JSON result"
            )
        try:
            data = _read_result_json(result_path)
            return _probe_result_from_data(
                data,
                binary_fingerprint=binary_fingerprint,
                stdout=completed.stdout,
                stderr=completed.stderr,
            )
        except (
            OSError,
            UnicodeError,
            ValueError,
            TypeError,
            KeyError,
            RecursionError,
        ) as error:
            raise RuntimeProbeError(
                f"gem5 catalog probe returned invalid JSON: {error}"
            ) from error


def merge_runtime_probe(
    catalog: Catalog, probe: RuntimeProbeResult
) -> Catalog:
    """Merge authoritative runtime import results into a source catalog."""

    if catalog.source_fingerprint != probe.source_fingerprint:
        raise ValueError(
            "runtime probe was produced for different gem5 sources"
        )
    runtime_symbols = {symbol.type_id: symbol for symbol in probe.symbols}
    entries: list[CatalogEntry] = []
    for entry in catalog.entries:
        runtime = runtime_symbols.get(entry.type_id)
        if runtime is None:
            diagnostic = CatalogDiagnostic(
                "catalog.probe_result_missing",
                "Runtime probe did not return this source symbol",
                DiagnosticSeverity.WARNING,
                entry.symbol.source_path,
                entry.symbol.line,
                entry.type_id,
            )
            entries.append(
                replace(
                    entry,
                    support_status=SupportStatus.SOURCE_ONLY,
                    diagnostics=(*entry.diagnostics, diagnostic),
                )
            )
            continue
        if not runtime.available:
            diagnostic = CatalogDiagnostic(
                "catalog.runtime_unavailable",
                runtime.error or "The symbol could not be imported by gem5",
                DiagnosticSeverity.WARNING,
                entry.symbol.source_path,
                entry.symbol.line,
                entry.type_id,
            )
            entries.append(
                replace(
                    entry,
                    support_status=SupportStatus.UNAVAILABLE,
                    diagnostics=(*entry.diagnostics, diagnostic),
                )
            )
            continue
        entries.append(
            replace(
                entry,
                support_status=SupportStatus.RUNTIME_CONFIRMED,
                parameters=_merge_parameters(entry, runtime),
                is_abstract=(
                    runtime.is_abstract
                    if runtime.is_abstract is not None
                    else entry.is_abstract
                ),
            )
        )
    return Catalog(
        source_root=catalog.source_root,
        entries=tuple(entries),
        source_fingerprint=catalog.source_fingerprint,
        diagnostics=(*catalog.diagnostics, *probe.diagnostics),
        binary_fingerprint=probe.binary_fingerprint,
        supported_isas=probe.supported_isas,
        supported_protocols=probe.supported_protocols,
    )


def _merge_parameters(
    entry: CatalogEntry, runtime: RuntimeSymbol
) -> tuple[ParameterSpec, ...]:
    runtime_by_id = {
        parameter.id: parameter for parameter in runtime.parameters
    }
    merged: list[ParameterSpec] = []
    source_ids: set[str] = set()
    for parameter in entry.parameters:
        source_ids.add(parameter.id)
        observed = runtime_by_id.get(parameter.id)
        if observed is None:
            merged.append(parameter)
            continue
        merged.append(
            replace(
                parameter,
                annotation=_richer_annotation(
                    parameter.annotation, observed.annotation
                ),
                kind=(
                    ParameterKind.ENUM if observed.choices else parameter.kind
                ),
                choices=observed.choices or parameter.choices,
                has_default=observed.has_default,
                required=not observed.has_default and not parameter.variadic,
                default_expression=(
                    observed.default_expression
                    if observed.has_default
                    else parameter.default_expression
                ),
                positional_only=observed.positional_only,
                keyword_only=observed.keyword_only,
                variadic=observed.variadic,
            )
        )
    for observed in runtime.parameters:
        if observed.id in source_ids:
            continue
        merged.append(
            ParameterSpec(
                id=observed.id,
                kind=(
                    ParameterKind.ENUM
                    if observed.choices
                    else ParameterKind.UNKNOWN
                ),
                annotation=observed.annotation,
                required=not observed.has_default and not observed.variadic,
                has_default=observed.has_default,
                default_expression=observed.default_expression,
                choices=observed.choices,
                positional_only=observed.positional_only,
                keyword_only=observed.keyword_only,
                variadic=observed.variadic,
            )
        )
    return tuple(merged)


def _richer_annotation(
    source_annotation: str | None, runtime_annotation: str | None
) -> str | None:
    """Prefer whichever annotation retains more structural type detail."""

    if not runtime_annotation:
        return source_annotation
    if not source_annotation:
        return runtime_annotation

    def identifiers(annotation: str) -> set[str]:
        return set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", annotation))

    def detail(annotation: str) -> int:
        structure = sum(annotation.count(character) for character in "[|,")
        return len(identifiers(annotation)) + structure

    if identifiers(source_annotation).intersection(
        identifiers(runtime_annotation)
    ) and detail(source_annotation) > detail(runtime_annotation):
        return source_annotation
    return runtime_annotation


def _probe_result_from_data(
    data: object,
    *,
    binary_fingerprint: str,
    stdout: str,
    stderr: str,
) -> RuntimeProbeResult:
    if not isinstance(data, dict):
        raise ValueError("unsupported runtime probe schema")
    schema_version = data.get("schema_version")
    if (
        isinstance(schema_version, bool)
        or not isinstance(schema_version, int)
        or schema_version != 1
    ):
        raise ValueError("unsupported runtime probe schema")
    source_fingerprint = _required_string(data, "source_fingerprint")
    raw_symbols = data.get("symbols")
    if not isinstance(raw_symbols, list):
        raise ValueError("symbols must be a list")
    symbols: list[RuntimeSymbol] = []
    for raw_symbol in raw_symbols:
        if not isinstance(raw_symbol, dict):
            raise ValueError("symbol results must be objects")
        raw_parameters = raw_symbol.get("parameters", [])
        if not isinstance(raw_parameters, list):
            raise ValueError("symbol parameters must be a list")
        parameters: list[RuntimeParameter] = []
        for raw_parameter in raw_parameters:
            if not isinstance(raw_parameter, dict):
                raise ValueError("runtime parameters must be objects")
            parameters.append(
                RuntimeParameter(
                    id=_required_string(raw_parameter, "id"),
                    annotation=_optional_string(raw_parameter, "annotation"),
                    choices=_string_tuple(raw_parameter.get("choices", [])),
                    has_default=_required_bool(raw_parameter, "has_default"),
                    default_expression=_optional_string(
                        raw_parameter, "default_expression"
                    ),
                    positional_only=_required_bool(
                        raw_parameter, "positional_only"
                    ),
                    keyword_only=_required_bool(raw_parameter, "keyword_only"),
                    variadic=_required_bool(raw_parameter, "variadic"),
                )
            )
        raw_kind = raw_symbol.get("symbol_kind")
        symbol_kind = SymbolKind(raw_kind) if raw_kind is not None else None
        symbols.append(
            RuntimeSymbol(
                type_id=_required_string(raw_symbol, "type_id"),
                available=_required_bool(raw_symbol, "available"),
                symbol_kind=symbol_kind,
                parameters=tuple(parameters),
                error=_optional_string(raw_symbol, "error"),
                is_abstract=_optional_bool(raw_symbol, "is_abstract"),
            )
        )
    return RuntimeProbeResult(
        source_fingerprint=source_fingerprint,
        binary_fingerprint=binary_fingerprint,
        symbols=tuple(symbols),
        supported_isas=_string_tuple(data.get("supported_isas", [])),
        supported_protocols=_string_tuple(data.get("supported_protocols", [])),
        stdout=stdout,
        stderr=stderr,
    )


def _required_string(data: dict[str, object], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"{key} must be a non-empty string")
    return value


def _optional_string(data: dict[str, object], key: str) -> str | None:
    value = data.get(key)
    if value is not None and not isinstance(value, str):
        raise ValueError(f"{key} must be a string or null")
    return value


def _required_bool(data: dict[str, object], key: str) -> bool:
    value = data.get(key)
    if not isinstance(value, bool):
        raise ValueError(f"{key} must be a boolean")
    return value


def _optional_bool(data: dict[str, object], key: str) -> bool | None:
    value = data.get(key)
    if value is not None and not isinstance(value, bool):
        raise ValueError(f"{key} must be a boolean or null")
    return value


def _string_tuple(value: object) -> tuple[str, ...]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) for item in value
    ):
        raise ValueError("expected a list of strings")
    return tuple(value)
