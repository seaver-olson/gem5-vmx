"""Asynchronous, single-job build and simulation controller."""

from __future__ import annotations

import math
import os
import shutil
import signal
import subprocess
import threading
from pathlib import Path
from typing import (
    Any,
    Callable,
)

from workbench.execution.artifacts import (
    ArtifactBundle,
    prepare_build_artifacts,
    prepare_run_artifacts,
    update_manifest,
)
from workbench.execution.installation import installation_fingerprint
from workbench.execution.models import (
    SUPPORTED_COMPONENT_TYPE_IDS,
    BuildRequest,
    JobKind,
    JobSnapshot,
    JobState,
    RunHandle,
    RunRequest,
)
from workbench.execution.translation import translate_project


class ExecutionControllerError(RuntimeError):
    """Raised when a job cannot be started."""


ProcessFactory = Callable[..., Any]
MAX_BUILD_JOBS = 1024


def default_build_jobs(environment: dict[str, str] | None = None) -> int:
    environ = os.environ if environment is None else environment
    configured = environ.get("GEM5_BUILD_JOBS")
    if configured is not None:
        try:
            value = int(configured)
        except ValueError as error:
            raise ExecutionControllerError(
                "GEM5_BUILD_JOBS must be a positive integer"
            ) from error
        if value < 1 or value > MAX_BUILD_JOBS:
            raise ExecutionControllerError(
                f"GEM5_BUILD_JOBS must be between 1 and {MAX_BUILD_JOBS}"
            )
        return value
    return min(8, os.cpu_count() or 1)


class AsyncExecutionController:
    """Own one non-blocking build or simulation process at a time."""

    def __init__(
        self,
        *,
        output_root: Path | None = None,
        process_factory: ProcessFactory | None = None,
        stop_grace_seconds: float = 2.0,
        tail_bytes: int = 32 * 1024,
    ) -> None:
        if (
            isinstance(stop_grace_seconds, bool)
            or not isinstance(stop_grace_seconds, (int, float))
            or not math.isfinite(stop_grace_seconds)
        ):
            raise ValueError("stop grace seconds must be a finite number")
        if isinstance(tail_bytes, bool) or not isinstance(tail_bytes, int):
            raise ValueError("tail bytes must be an integer")
        self._output_root = output_root
        self._process_factory = process_factory or subprocess.Popen
        self._native_process = process_factory is None
        self._stop_grace_seconds = max(0.01, stop_grace_seconds)
        self._tail_bytes = min(1024 * 1024, max(1024, tail_bytes))
        self._lock = threading.RLock()
        self._manifest_lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._process: Any | None = None
        self._stop_requested = threading.Event()
        self._escalation_started = False
        self._job_id: str | None = None
        self._kind: JobKind | None = None
        self._state = JobState.IDLE
        self._bundle: ArtifactBundle | None = None
        self._command: list[str] | None = None
        self._return_code: int | None = None
        self._error: str | None = None

    def _root_for(self, requested: Path | None, repo_root: Path) -> Path:
        return (
            requested
            or self._output_root
            or repo_root / "util/gem5-workbench/run-output"
        ).resolve()

    def _ensure_available(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            raise ExecutionControllerError(
                "another gem5 job is still being finalized"
            )
        if self._state not in {
            JobState.IDLE,
            JobState.SUCCEEDED,
            JobState.FAILED,
            JobState.CANCELLED,
        }:
            raise ExecutionControllerError(
                "another gem5 job is already active"
            )

    def start_run(self, request: RunRequest) -> RunHandle:
        installation = request.installation
        binary = installation.binary
        if binary is None or not binary.is_file():
            raise ExecutionControllerError(
                "Run requires a built gem5 binary; use Build first"
            )
        if request.probe is None:
            raise ExecutionControllerError(
                "Run requires a successful probe of the selected gem5 binary"
            )
        try:
            current_fingerprint = installation_fingerprint(installation)
        except OSError as error:
            raise ExecutionControllerError(
                f"could not fingerprint the selected gem5 binary: {error}"
            ) from error
        if request.probe.binary_fingerprint != current_fingerprint:
            raise ExecutionControllerError(
                "the gem5 binary changed after it was probed; probe it again"
            )
        if not request.probe.supports_x86:
            raise ExecutionControllerError(
                "the selected gem5 binary does not include the X86 ISA"
            )
        missing_type_ids = sorted(
            SUPPORTED_COMPONENT_TYPE_IDS
            - set(request.probe.importable_type_ids)
        )
        if missing_type_ids:
            raise ExecutionControllerError(
                "the selected gem5 binary cannot import required component "
                "type(s): " + ", ".join(missing_type_ids)
            )
        try:
            project_directory = (
                Path(request.project_path).expanduser().resolve().parent
                if request.project_path is not None
                else None
            )
            plan = translate_project(
                request.document,
                installation,
                project_directory=project_directory,
            )
        except (OSError, RuntimeError, TypeError, ValueError) as error:
            raise ExecutionControllerError(
                f"could not translate the project: {error}"
            ) from error
        workload = plan.workload["parameters"]
        if (
            workload["kind"] == "local"
            and not Path(workload["path"]).is_file()
        ):
            raise ExecutionControllerError(
                f"local workload does not exist: {workload['path']}"
            )
        with self._lock:
            self._ensure_available()
            environment = dict(os.environ)
            bridge = Path(__file__).with_name("_bridge.py").resolve()
            try:
                bundle = prepare_run_artifacts(
                    self._root_for(
                        request.output_root, installation.repo_root
                    ),
                    request.project_name,
                    request.document,
                    plan,
                    installation,
                    request.catalog_fingerprint,
                    bridge_source=bridge,
                    environment=environment,
                )
            except Exception as error:
                raise ExecutionControllerError(
                    f"could not prepare run artifacts: {error}"
                ) from error
            try:
                prepared_fingerprint = installation_fingerprint(installation)
            except OSError as error:
                shutil.rmtree(bundle.directory, ignore_errors=True)
                raise ExecutionControllerError(
                    "could not fingerprint the selected gem5 binary after "
                    f"preparing the run: {error}"
                ) from error
            if prepared_fingerprint != current_fingerprint:
                shutil.rmtree(bundle.directory, ignore_errors=True)
                raise ExecutionControllerError(
                    "the gem5 binary changed while the run was being prepared; "
                    "probe it again"
                )
            assert bundle.bridge_path is not None
            command = [
                str(binary),
                "-q",
                "--listener-mode=off",
                "-d",
                str(bundle.directory),
                str(bundle.bridge_path),
                "--spec",
                str(bundle.spec_path),
            ]
            return self._start(
                JobKind.RUN,
                JobState.RUNNING,
                bundle,
                command,
                installation.repo_root,
                environment,
            )

    def start_build(self, request: BuildRequest) -> RunHandle:
        installation = request.installation
        jobs = (
            request.jobs if request.jobs is not None else default_build_jobs()
        )
        if (
            isinstance(jobs, bool)
            or not isinstance(jobs, int)
            or not 1 <= jobs <= MAX_BUILD_JOBS
        ):
            raise ExecutionControllerError(
                f"build jobs must be between 1 and {MAX_BUILD_JOBS}"
            )
        with self._lock:
            self._ensure_available()
            environment = dict(os.environ)
            try:
                bundle = prepare_build_artifacts(
                    self._root_for(
                        request.output_root, installation.repo_root
                    ),
                    request.project_name,
                    installation,
                    environment=environment,
                )
            except Exception as error:
                raise ExecutionControllerError(
                    f"could not prepare build artifacts: {error}"
                ) from error
            scons = (
                shutil.which("scons", path=environment.get("PATH")) or "scons"
            )
            command = [scons, installation.build_target, f"-j{jobs}"]
            return self._start(
                JobKind.BUILD,
                JobState.BUILDING,
                bundle,
                command,
                installation.repo_root,
                environment,
            )

    def _start(
        self,
        kind: JobKind,
        active_state: JobState,
        bundle: ArtifactBundle,
        command: list[str],
        cwd: Path,
        environment: dict[str, str],
    ) -> RunHandle:
        self._stop_requested = threading.Event()
        self._escalation_started = False
        self._job_id = bundle.job_id
        self._kind = kind
        self._state = JobState.STARTING
        self._bundle = bundle
        self._command = list(command)
        self._return_code = None
        self._error = None
        try:
            self._record_manifest(
                bundle, state=JobState.STARTING, command=command
            )
            thread = threading.Thread(
                target=self._worker,
                args=(
                    kind,
                    active_state,
                    list(command),
                    cwd,
                    dict(environment),
                    bundle,
                ),
                name=f"gem5-workbench-{kind.value}-{bundle.job_id}",
                daemon=True,
            )
            self._thread = thread
            thread.start()
        except Exception as error:
            self._thread = None
            self._process = None
            self._state = JobState.FAILED
            self._error = f"could not start job worker: {error}"
            try:
                self._record_manifest(
                    bundle,
                    state=JobState.FAILED,
                    command=command,
                    error=self._error,
                )
            except Exception:
                pass
            raise ExecutionControllerError(self._error) from error
        return RunHandle(bundle.job_id, kind, bundle.directory)

    def _worker(
        self,
        kind: JobKind,
        active_state: JobState,
        command: list[str],
        cwd: Path,
        environment: dict[str, str],
        bundle: ArtifactBundle,
    ) -> None:
        process: Any | None = None
        try:
            with bundle.log_path.open("ab", buffering=0) as log:
                kwargs = {
                    "cwd": cwd,
                    "stdin": subprocess.DEVNULL,
                    "stdout": log,
                    "stderr": subprocess.STDOUT,
                    "env": environment,
                }
                if self._native_process:
                    kwargs["start_new_session"] = True
                process = self._process_factory(command, **kwargs)
                with self._lock:
                    self._process = process
                    if self._stop_requested.is_set():
                        self._state = JobState.STOPPING
                    else:
                        self._state = active_state
                    state = self._state
                try:
                    self._record_manifest(bundle, state=state, command=command)
                except Exception:
                    # The process must remain managed even if a later status
                    # update cannot be persisted.
                    pass
                if self._stop_requested.is_set():
                    self._schedule_stop(process)
                return_code = process.wait()
            with self._lock:
                self._return_code = return_code
                if self._stop_requested.is_set():
                    self._state = JobState.CANCELLED
                elif return_code == 0:
                    artifact_error = (
                        self._run_artifact_error(bundle)
                        if kind is JobKind.RUN
                        else None
                    )
                    if artifact_error is None:
                        self._state = JobState.SUCCEEDED
                    else:
                        self._state = JobState.FAILED
                        self._error = artifact_error
                else:
                    self._state = JobState.FAILED
                    self._error = f"process exited with status {return_code}"
                state = self._state
                error = self._error
            try:
                self._record_manifest(
                    bundle,
                    state=state,
                    command=command,
                    return_code=return_code,
                    error=error,
                )
            except Exception as manifest_error:
                with self._lock:
                    self._state = JobState.FAILED
                    self._error = (
                        "process finished, but its manifest could not be "
                        f"updated: {manifest_error}"
                    )
        # Process failures are reported through state instead of escaping the
        # worker and taking down the caller's event loop.
        except Exception as error:
            self._kill_process_after_worker_error(process)
            with self._lock:
                self._state = (
                    JobState.CANCELLED
                    if self._stop_requested.is_set()
                    else JobState.FAILED
                )
                self._error = str(error)
                state = self._state
            try:
                self._record_manifest(
                    bundle, state=state, command=command, error=str(error)
                )
            except Exception:
                pass
        finally:
            with self._lock:
                if self._process is process:
                    self._process = None

    def _kill_process_after_worker_error(self, process: Any | None) -> None:
        """Best-effort cleanup when process supervision itself fails."""

        if process is None:
            return
        try:
            return_code = getattr(process, "returncode", None)
            group_alive = self._process_group_alive(process)
        except Exception:
            return_code = None
            group_alive = True
        if return_code is not None and not group_alive:
            return
        try:
            self._signal_process(process, signal.SIGKILL)
        except Exception:
            return
        try:
            process.wait(timeout=min(1.0, self._stop_grace_seconds))
        except Exception:
            pass

    @staticmethod
    def _run_artifact_error(bundle: ArtifactBundle) -> str | None:
        missing = []
        for name in ("stats.txt", "config.ini", "config.json"):
            path = bundle.directory / name
            try:
                valid = path.is_file() and path.stat().st_size > 0
            except OSError:
                valid = False
            if not valid:
                missing.append(name)
        if not missing:
            return None
        return (
            "gem5 exited successfully without nonempty required artifact(s): "
            + ", ".join(missing)
        )

    def _signal_process(self, process: Any, requested_signal: int) -> None:
        try:
            pid = getattr(process, "pid", None)
            if self._native_process and isinstance(pid, int) and pid > 0:
                os.killpg(pid, requested_signal)
            elif requested_signal == signal.SIGTERM:
                process.terminate()
            elif requested_signal == signal.SIGKILL:
                process.kill()
            else:
                process.send_signal(requested_signal)
        except (AttributeError, OSError, ProcessLookupError):
            return

    def _process_group_alive(self, process: Any) -> bool:
        if not self._native_process:
            return False
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

    def _schedule_stop(self, process: Any) -> None:
        with self._lock:
            if process is not self._process or self._escalation_started:
                return
            self._escalation_started = True
        self._signal_process(process, signal.SIGINT)
        escalation = threading.Thread(
            target=self._escalate_stop,
            args=(process,),
            name="gem5-workbench-stop",
            daemon=True,
        )
        try:
            escalation.start()
        except RuntimeError:
            # Thread exhaustion must not strand a process after Stop was
            # accepted. Escalate immediately without blocking the caller.
            self._signal_process(process, signal.SIGTERM)
            self._signal_process(process, signal.SIGKILL)

    def request_stop(self) -> bool:
        with self._lock:
            if self._state in {
                JobState.IDLE,
                JobState.SUCCEEDED,
                JobState.FAILED,
                JobState.CANCELLED,
            }:
                return False
            self._stop_requested.set()
            self._state = JobState.STOPPING
            process = self._process
            bundle = self._bundle
            command = self._command
        if bundle is not None:
            try:
                self._record_manifest(
                    bundle, state=JobState.STOPPING, command=command
                )
            except Exception:
                pass
        if process is not None:
            self._schedule_stop(process)
        return True

    def _escalate_stop(self, process: Any) -> None:
        try:
            process.wait(timeout=self._stop_grace_seconds)
        except (OSError, subprocess.TimeoutExpired):
            pass
        else:
            if not self._process_group_alive(process):
                return
        self._signal_process(process, signal.SIGTERM)
        try:
            process.wait(timeout=self._stop_grace_seconds)
        except (OSError, subprocess.TimeoutExpired):
            pass
        else:
            if not self._process_group_alive(process):
                return
        self._signal_process(process, signal.SIGKILL)

    def poll(self) -> JobSnapshot:
        with self._lock:
            bundle = self._bundle
            snapshot = JobSnapshot(
                job_id=self._job_id,
                kind=self._kind,
                state=self._state,
                output_dir=bundle.directory if bundle else None,
                return_code=self._return_code,
                error=self._error,
            )
        return JobSnapshot(
            job_id=snapshot.job_id,
            kind=snapshot.kind,
            state=snapshot.state,
            output_dir=snapshot.output_dir,
            return_code=snapshot.return_code,
            error=snapshot.error,
            recent_output=self._tail(bundle.log_path) if bundle else "",
        )

    def wait(self, timeout: float | None = None) -> JobSnapshot:
        with self._lock:
            thread = self._thread
        if thread is not None:
            thread.join(timeout)
        return self.poll()

    def _tail(self, path: Path) -> str:
        try:
            with path.open("rb") as stream:
                stream.seek(0, os.SEEK_END)
                size = stream.tell()
                stream.seek(max(0, size - self._tail_bytes))
                return stream.read().decode("utf-8", errors="replace")
        except OSError:
            return ""

    def _record_manifest(self, bundle: ArtifactBundle, **values: Any) -> None:
        with self._manifest_lock:
            update_manifest(bundle, **values)
