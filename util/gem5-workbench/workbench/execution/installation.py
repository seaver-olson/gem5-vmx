"""Discovery and isolated probing of a local gem5 installation."""

from __future__ import annotations

import hashlib
import json
import math
import os
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import (
    Mapping,
    Sequence,
)

from workbench.execution.models import (
    Gem5Installation,
    Gem5Probe,
)

ROOT_ENVIRONMENT_VARIABLE = "GEM5_WORKBENCH_GEM5_ROOT"
BINARY_ENVIRONMENT_VARIABLE = "GEM5_WORKBENCH_GEM5_BINARY"


class Gem5DiscoveryError(ValueError):
    """Raised when a usable gem5 source tree cannot be located."""


class Gem5ProbeError(RuntimeError):
    """Raised when a gem5 binary cannot be probed safely."""


_FINGERPRINT_CACHE: dict[tuple[str, int, int, int], str] = {}
_FINGERPRINT_CACHE_LOCK = threading.Lock()
_MAX_PROBE_RESULT_BYTES = 1024 * 1024
_PROBE_LOG_TAIL_BYTES = 64 * 1024


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON object key: {key}")
        value[key] = item
    return value


def _read_limited_bytes(path: Path, limit: int) -> bytes:
    with path.open("rb") as stream:
        value = stream.read(limit + 1)
    if len(value) > limit:
        raise ValueError(f"probe result exceeds {limit} bytes")
    return value


def _read_log_tail(stream: object, limit: int) -> str:
    try:
        stream.flush()  # type: ignore[attr-defined]
        stream.seek(0, os.SEEK_END)  # type: ignore[attr-defined]
        size = stream.tell()  # type: ignore[attr-defined]
        stream.seek(max(0, size - limit))  # type: ignore[attr-defined]
        value = stream.read()  # type: ignore[attr-defined]
    except (AttributeError, OSError, ValueError):
        return ""
    if isinstance(value, str):
        return value
    return bytes(value).decode("utf-8", errors="replace")


def _is_repo_root(path: Path) -> bool:
    return (
        path.is_dir()
        and (path / "SConstruct").is_file()
        and (path / "src/python/gem5").is_dir()
    )


def _walk_for_root(start: Path) -> Path | None:
    candidate = start.resolve()
    if candidate.is_file():
        candidate = candidate.parent
    for parent in (candidate, *candidate.parents):
        if _is_repo_root(parent):
            return parent
    return None


def _binary_candidates(root: Path) -> tuple[Path, ...]:
    return tuple(
        root / relative
        for relative in (
            "build/X86/gem5.opt",
            "build/ALL/gem5.opt",
            "build/X86/gem5.fast",
            "build/ALL/gem5.fast",
            "build/X86/gem5.debug",
            "build/ALL/gem5.debug",
        )
    )


def _git_revision(root: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    revision = result.stdout.strip()
    return revision if result.returncode == 0 and revision else None


def discover_gem5_installation(
    *,
    repo_root: str | Path | None = None,
    binary: str | Path | None = None,
    start: str | Path | None = None,
    environment: Mapping[str, str] | None = None,
) -> Gem5Installation:
    """Locate gem5 using explicit values, environment, this tree, then PATH."""

    environ = os.environ if environment is None else environment
    requested_root = repo_root or environ.get(ROOT_ENVIRONMENT_VARIABLE)
    requested_binary = binary or environ.get(BINARY_ENVIRONMENT_VARIABLE)

    root: Path | None = None
    if requested_root is not None:
        root = Path(requested_root).expanduser().resolve()
        if not _is_repo_root(root):
            raise Gem5DiscoveryError(f"not a gem5 repository: {root}")

    executable: Path | None = None
    if requested_binary is not None:
        executable = Path(requested_binary).expanduser().resolve()
        if not executable.is_file():
            raise Gem5DiscoveryError(
                f"gem5 binary does not exist: {executable}"
            )
        if root is None:
            root = _walk_for_root(executable)

    if root is None:
        search_start = Path(start) if start is not None else Path(__file__)
        root = _walk_for_root(search_start)

    if root is not None and executable is None:
        executable = next(
            (
                candidate
                for candidate in _binary_candidates(root)
                if candidate.is_file()
            ),
            None,
        )

    if executable is None:
        search_path = environ.get("PATH", "")
        path_binary = shutil.which(
            "gem5.opt", path=search_path
        ) or shutil.which("gem5", path=search_path)
        if path_binary:
            executable = Path(path_binary).resolve()
            if root is None:
                root = _walk_for_root(executable)

    if root is None:
        raise Gem5DiscoveryError(
            "could not locate a gem5 repository; set "
            f"{ROOT_ENVIRONMENT_VARIABLE}"
        )

    return Gem5Installation(
        repo_root=root,
        binary=executable,
        source_revision=_git_revision(root),
    )


def fingerprint_file(path: Path) -> str:
    """Return a cached SHA-256 content identity for a regular file."""

    resolved = path.resolve()
    stat = resolved.stat()
    key = (
        str(resolved),
        stat.st_size,
        stat.st_mtime_ns,
        stat.st_ctime_ns,
    )
    with _FINGERPRINT_CACHE_LOCK:
        cached = _FINGERPRINT_CACHE.get(key)
    if cached is not None:
        return cached

    digest = hashlib.sha256()
    with resolved.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    value = digest.hexdigest()
    with _FINGERPRINT_CACHE_LOCK:
        # Bound this process-local optimization; manifests retain the hashes.
        if len(_FINGERPRINT_CACHE) >= 64:
            _FINGERPRINT_CACHE.clear()
        _FINGERPRINT_CACHE[key] = value
    return value


def installation_fingerprint(installation: Gem5Installation) -> str:
    """Return a content-based identity for the selected gem5 build."""

    binary_state: dict[str, object] | None = None
    if installation.binary is not None and installation.binary.is_file():
        binary_state = {
            "sha256": fingerprint_file(installation.binary),
            "size": installation.binary.stat().st_size,
        }
    state = {
        "revision": installation.source_revision,
        "build_target": installation.build_target,
        "binary": binary_state,
    }
    payload = json.dumps(state, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def probe_gem5_installation(
    installation: Gem5Installation,
    *,
    timeout: float = 30.0,
    extra_environment: Mapping[str, str] | None = None,
    cancel_event: threading.Event | None = None,
) -> Gem5Probe:
    """Run the fixed probe config through gem5 without instantiating objects."""

    if (
        isinstance(timeout, bool)
        or not isinstance(timeout, (int, float))
        or not math.isfinite(timeout)
        or timeout <= 0
    ):
        raise ValueError("probe timeout must be positive")
    if cancel_event is not None and cancel_event.is_set():
        raise Gem5ProbeError("gem5 probe was cancelled")
    binary = installation.binary
    if binary is None or not binary.is_file():
        raise Gem5ProbeError("a built gem5 binary is required for probing")
    try:
        fingerprint_before = installation_fingerprint(installation)
    except OSError as error:
        raise Gem5ProbeError(
            f"could not fingerprint the gem5 binary: {error}"
        ) from error
    probe_script = Path(__file__).with_name("_probe.py")
    try:
        temporary_directory = tempfile.TemporaryDirectory(
            prefix="gem5-workbench-probe-"
        )
    except OSError as error:
        raise Gem5ProbeError(
            f"could not create a gem5 probe workspace: {error}"
        ) from error
    with temporary_directory as temp:
        temporary = Path(temp)
        output = temporary / "probe.json"
        probe_log = temporary / "probe.log"
        command: Sequence[str] = (
            str(binary),
            "-q",
            "--listener-mode=off",
            "-d",
            str(temporary / "m5out"),
            str(probe_script),
            "--output",
            str(output),
        )
        process_environment = os.environ.copy()
        if extra_environment:
            try:
                process_environment.update(extra_environment)
            except (TypeError, ValueError) as error:
                raise Gem5ProbeError(
                    f"invalid gem5 probe environment: {error}"
                ) from error
        try:
            log_stream = probe_log.open("w+b")
        except OSError as error:
            raise Gem5ProbeError(
                f"could not create the gem5 probe log: {error}"
            ) from error
        with log_stream:
            try:
                process = subprocess.Popen(
                    command,
                    cwd=installation.repo_root,
                    env=process_environment,
                    stdin=subprocess.DEVNULL,
                    stdout=log_stream,
                    stderr=subprocess.STDOUT,
                    text=False,
                    start_new_session=True,
                )
            except (OSError, TypeError, ValueError) as error:
                raise Gem5ProbeError(
                    f"could not start gem5 probe: {error}"
                ) from error

            deadline = time.monotonic() + timeout
            stop_attempted = False
            try:
                while True:
                    if cancel_event is not None and cancel_event.is_set():
                        stop_attempted = True
                        _stop_probe_process(process)
                        raise Gem5ProbeError("gem5 probe was cancelled")
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        stop_attempted = True
                        _stop_probe_process(process)
                        raise Gem5ProbeError(
                            f"gem5 probe timed out after {timeout:g} seconds"
                        )
                    try:
                        process.wait(timeout=min(0.1, remaining))
                        break
                    except subprocess.TimeoutExpired:
                        continue
                    except OSError as error:
                        stop_attempted = True
                        _stop_probe_process(process)
                        raise Gem5ProbeError(
                            f"could not wait for gem5 probe: {error}"
                        ) from error
            except BaseException:
                if not stop_attempted and (
                    process.returncode is None
                    or _probe_process_group_alive(process)
                ):
                    _stop_probe_process(process)
                raise

            if process.returncode != 0:
                detail = _read_log_tail(log_stream, _PROBE_LOG_TAIL_BYTES)
                detail = detail.strip()
                raise Gem5ProbeError(
                    f"gem5 probe exited with {process.returncode}"
                    + (f": {detail[-1000:]}" if detail else "")
                )
        try:
            data = json.loads(
                _read_limited_bytes(output, _MAX_PROBE_RESULT_BYTES).decode(
                    "utf-8"
                ),
                object_pairs_hook=_unique_json_object,
            )
            if not isinstance(data, dict):
                raise ValueError("probe result must be an object")
            expected_keys = {
                "version",
                "supported_isas",
                "supported_protocols",
                "importable_type_ids",
            }
            if set(data) != expected_keys:
                raise ValueError("probe result has invalid fields")
            version = data["version"]
            if (
                not isinstance(version, str)
                or not version
                or version != version.strip()
            ):
                raise ValueError("version must be a non-empty string")
            sequences = {}
            for key in (
                "supported_isas",
                "supported_protocols",
                "importable_type_ids",
            ):
                values = data[key]
                if not isinstance(values, list) or not all(
                    isinstance(value, str) and value and value == value.strip()
                    for value in values
                ):
                    raise ValueError(
                        f"{key} must be a list of non-empty strings"
                    )
                sequences[key] = tuple(sorted(set(values)))
            isas = sequences["supported_isas"]
            protocols = sequences["supported_protocols"]
            importable = sequences["importable_type_ids"]
        except (
            OSError,
            UnicodeError,
            KeyError,
            TypeError,
            ValueError,
            RecursionError,
            json.JSONDecodeError,
        ) as error:
            raise Gem5ProbeError(
                "gem5 probe returned malformed data"
            ) from error

    try:
        fingerprint_after = installation_fingerprint(installation)
    except OSError as error:
        raise Gem5ProbeError(
            f"could not fingerprint the gem5 binary after probing: {error}"
        ) from error
    if fingerprint_after != fingerprint_before:
        raise Gem5ProbeError("gem5 binary changed while it was being probed")

    return Gem5Probe(
        version=version,
        supported_isas=isas,
        supported_protocols=protocols,
        importable_type_ids=importable,
        binary_fingerprint=fingerprint_after,
    )


def _signal_probe_process(
    process: subprocess.Popen[bytes], value: int
) -> None:
    try:
        pid = getattr(process, "pid", None)
        if isinstance(pid, int) and pid > 0:
            os.killpg(pid, value)
        else:
            process.send_signal(value)
    except (AttributeError, OSError, ProcessLookupError):
        pass


def _probe_process_group_alive(process: subprocess.Popen[bytes]) -> bool:
    pid = getattr(process, "pid", None)
    if not isinstance(pid, int) or pid <= 0:
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


def _stop_probe_process(process: subprocess.Popen[bytes]) -> None:
    for value in (signal.SIGINT, signal.SIGTERM):
        _signal_probe_process(process, value)
        try:
            process.wait(timeout=0.5)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if not _probe_process_group_alive(process):
            return
    _signal_probe_process(process, signal.SIGKILL)
    try:
        process.wait(timeout=0.5)
    except (OSError, subprocess.TimeoutExpired):
        pass
