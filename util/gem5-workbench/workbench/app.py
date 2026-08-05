"""Main pygame application for the gem5 Workbench."""

from __future__ import annotations

import os
import queue
import threading
from pathlib import Path

import pygame
from workbench.catalog import (
    Catalog,
    CatalogCache,
    discover_source_catalog,
    fingerprint_file,
    fingerprint_source_tree,
    merge_runtime_probe,
    run_runtime_probe,
)
from workbench.constants import (
    INITIAL_WINDOW_SIZE,
    MINIMUM_WINDOW_SIZE,
    TARGET_FPS,
    WIN_TITLE,
)
from workbench.document import ProjectDocument
from workbench.execution import (
    SUPPORTED_COMPONENT_TYPE_IDS,
    AsyncExecutionController,
    BuildRequest,
    ExecutionControllerError,
    Gem5DiscoveryError,
    Gem5ProbeError,
    JobState,
    RunRequest,
    discover_gem5_installation,
    installation_fingerprint,
    probe_gem5_installation,
    translate_project,
)
from workbench.persistence import (
    ProjectFileError,
    load_project_document,
    save_project_document,
)
from workbench.registry import (
    ComponentSupport,
    create_builtin_registry,
    create_registry_from_catalog,
)
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.canvas import Canvas
from workbench.ui.inspector import Inspector
from workbench.ui.palette import Palette
from workbench.ui.theme import Theme
from workbench.ui.toolbar import Toolbar
from workbench.ui.widgets import (
    draw_text,
    ellipsize,
    get_font,
)
from workbench.ui.window_layout import create_layout
from workbench.validation import (
    DiagnosticSeverity,
    validate_project,
)

WORKBENCH_ROOT = Path(__file__).resolve().parent.parent
PROJECT_PATH = Path(
    os.environ.get(
        "GEM5_WORKBENCH_PROJECT",
        WORKBENCH_ROOT / "projects" / "local" / "current.g5proj",
    )
)


def _catalog_cache_directory() -> Path:
    configured = os.environ.get("XDG_CACHE_HOME")
    root = Path(configured) if configured else Path.home() / ".cache"
    return root / "gem5-workbench" / "catalog"


class WorkbenchApp:
    """Own the pygame lifecycle, catalog, document, jobs, and UI panels."""

    def __init__(
        self,
        *,
        project_path: str | Path | None = None,
        gem5_root: str | Path | None = None,
        gem5_binary: str | Path | None = None,
        auto_probe: bool = True,
    ) -> None:
        pygame.init()
        pygame.display.set_caption(WIN_TITLE)
        self.surface = pygame.display.set_mode(
            INITIAL_WINDOW_SIZE, pygame.RESIZABLE
        )
        self.clock = pygame.time.Clock()
        self.theme = Theme()
        self.project_path = Path(project_path or PROJECT_PATH)
        self.state = WorkbenchState()
        self.installation = None
        self.execution_probe = None
        self.catalog: Catalog | None = None
        self._catalog_source_stale = False
        self.catalog_cache = CatalogCache(_catalog_cache_directory())
        self._catalog_updates: queue.SimpleQueue[
            tuple[
                int,
                Catalog | None,
                object | None,
                str | None,
                str | None,
            ]
        ] = queue.SimpleQueue()
        self._catalog_thread: threading.Thread | None = None
        self._catalog_generation = 0
        self._catalog_cancel = threading.Event()
        self.controller = AsyncExecutionController()
        self._handled_terminal_job_id: str | None = None
        self._job_poll_elapsed = 0.0

        try:
            self.installation = discover_gem5_installation(
                repo_root=gem5_root,
                binary=gem5_binary,
                start=WORKBENCH_ROOT,
            )
        except Gem5DiscoveryError as error:
            self.state.catalog_message = str(error)
            self.state.status_message = str(error)
        else:
            self.state.can_build = True
            if self.installation.binary is not None:
                self.state.gem5_binary = str(self.installation.binary)
            self._load_source_catalog()

        self.registry = (
            create_registry_from_catalog(self.catalog)
            if self.catalog is not None
            else create_builtin_registry()
        )
        self.window_layout = create_layout(self.surface.get_size())
        self.toolbar = Toolbar(
            self.window_layout.toolbar,
            self.theme,
            on_new=self.new_project,
            on_save=self.save_project,
            on_load=self.load_project,
            on_build=self.build_gem5,
            on_refresh=self.refresh_catalog,
            on_run=self.run_project,
            on_stop=self.stop_job,
        )
        self.palette = Palette(
            self.window_layout.palette, self.theme, self.registry
        )
        self.canvas = Canvas(
            self.window_layout.canvas, self.theme, self.registry
        )
        self.inspector = Inspector(
            self.window_layout.inspector, self.theme, self.registry
        )
        self.panels: tuple[Panel, ...] = (
            self.toolbar,
            self.palette,
            self.canvas,
            self.inspector,
        )
        self.running = False
        if self.project_path.is_file():
            self.load_project()
        else:
            self.refresh_validation()
        if (
            auto_probe
            and self.installation is not None
            and self.installation.binary is not None
        ):
            self.refresh_catalog()

    def _load_source_catalog(self) -> None:
        if self.installation is None:
            return
        try:
            self.catalog = discover_source_catalog(self.installation.repo_root)
            if (
                fingerprint_source_tree(self.installation.repo_root)
                != self.catalog.source_fingerprint
            ):
                raise RuntimeError(
                    "gem5 Python sources changed during catalog scan"
                )
        except (OSError, ValueError, RuntimeError) as error:
            self.catalog = None
            self.state.catalog_message = f"Catalog scan failed: {error}"
        else:
            warning = None
            try:
                self.catalog_cache.store(self.catalog)
            except (OSError, TypeError, ValueError) as error:
                warning = f"catalog cache unavailable: {error}"
            self.state.catalog_message = (
                f"Discovered {len(self.catalog.entries)} Standard Library symbols"
                + (f"; {warning}" if warning else "")
            )

    def _install_catalog(self, catalog: Catalog) -> None:
        self.catalog = catalog
        self._catalog_source_stale = False
        self.registry = create_registry_from_catalog(catalog)
        self.palette.set_registry(self.registry)
        self.canvas.set_registry(self.registry)
        self.inspector.set_registry(self.registry)
        self.canvas.ensure_components_visible(self.state)
        selected_definition = (
            self.registry.get(self.state.selected_type_id)
            if self.state.selected_type_id is not None
            else None
        )
        if (
            selected_definition is None
            or selected_definition.support is ComponentSupport.DEPRECATED
        ):
            self.state.selected_type_id = None
        confirmed = sum(
            entry.support_status.value == "runtime_confirmed"
            for entry in catalog.entries
        )
        detail = f"; {confirmed} confirmed by gem5" if confirmed else ""
        self.state.catalog_message = (
            f"Catalog contains {len(catalog.entries)} symbols{detail}"
        )
        self.refresh_validation()

    def refresh_catalog(self) -> None:
        if self.installation is None:
            self.state.status_message = "No gem5 repository is configured"
            return
        if self.state.catalog_busy or (
            self._catalog_thread is not None
            and self._catalog_thread.is_alive()
        ):
            self.state.status_message = "Catalog refresh already in progress"
            return
        snapshot = self.controller.poll()
        if snapshot.state is not JobState.IDLE and not snapshot.state.terminal:
            self.state.status_message = (
                "Wait for the active job before refreshing the catalog"
            )
            return
        installation = self.installation
        self._catalog_generation += 1
        generation = self._catalog_generation
        self._catalog_cancel.clear()
        # Never leave Run enabled against capability data which predates an
        # explicit refresh. A successful probe will restore readiness.
        self.execution_probe = None
        self.state.catalog_busy = True
        self.refresh_validation()
        self.state.catalog_message = "Refreshing catalog…"
        self.state.status_message = "Refreshing gem5 catalog"

        def worker() -> None:
            execution_probe = None
            binary_fingerprint: str | None = None
            should_store_catalog = False
            warnings: list[str] = []
            try:
                source = discover_source_catalog(installation.repo_root)
                catalog = source
                if installation.binary is not None:
                    binary_fingerprint = fingerprint_file(installation.binary)
                    cached = self.catalog_cache.load(
                        source.source_fingerprint,
                        binary_fingerprint,
                        expected_source_root=source.source_root,
                    )
                    if cached is not None:
                        catalog = cached
                    else:
                        result = run_runtime_probe(
                            installation.binary,
                            source,
                            binary_fingerprint=binary_fingerprint,
                            cancel_event=self._catalog_cancel,
                        )
                        catalog = merge_runtime_probe(source, result)
                        should_store_catalog = True
                else:
                    should_store_catalog = True
                if installation.binary is not None:
                    if self._catalog_cancel.is_set():
                        return
                    execution_probe = probe_gem5_installation(
                        installation, cancel_event=self._catalog_cancel
                    )
                if self._catalog_cancel.is_set():
                    return
                current_source_fingerprint = fingerprint_source_tree(
                    installation.repo_root
                )
                if current_source_fingerprint != source.source_fingerprint:
                    raise RuntimeError(
                        "gem5 Python sources changed during catalog refresh; "
                        "refresh again"
                    )
                if installation.binary is not None:
                    current_binary_fingerprint = fingerprint_file(
                        installation.binary
                    )
                    if current_binary_fingerprint != binary_fingerprint:
                        raise RuntimeError(
                            "gem5 binary changed during catalog refresh; "
                            "refresh again"
                        )
                if self._catalog_cancel.is_set():
                    return
                if should_store_catalog:
                    try:
                        self.catalog_cache.store(catalog)
                    except (OSError, TypeError, ValueError) as error:
                        warnings.append(f"catalog cache unavailable: {error}")
            except (OSError, TypeError, ValueError, RuntimeError) as error:
                self._catalog_updates.put(
                    (generation, None, None, str(error), None)
                )
            else:
                self._catalog_updates.put(
                    (
                        generation,
                        catalog,
                        execution_probe,
                        None,
                        "; ".join(warnings) if warnings else None,
                    )
                )

        self._catalog_thread = threading.Thread(
            target=worker,
            name="gem5-workbench-catalog",
            daemon=True,
        )
        try:
            self._catalog_thread.start()
        except RuntimeError as error:
            self._catalog_thread = None
            self.state.catalog_busy = False
            self.state.catalog_message = f"Catalog refresh failed: {error}"
            self.state.status_message = self.state.catalog_message

    def _consume_catalog_updates(self) -> None:
        while True:
            try:
                generation, catalog, execution_probe, error, warning = (
                    self._catalog_updates.get_nowait()
                )
            except queue.Empty:
                return
            if generation != self._catalog_generation:
                continue
            self.state.catalog_busy = False
            if error is not None:
                self.state.catalog_message = f"Catalog refresh failed: {error}"
                self.state.status_message = self.state.catalog_message
            elif catalog is not None:
                self.execution_probe = execution_probe
                self._install_catalog(catalog)
                if warning:
                    self.state.catalog_message += f"; {warning}"
                self.state.status_message = self.state.catalog_message

    def refresh_validation(self) -> None:
        self.state.diagnostics = validate_project(
            self.state.document, self.registry
        )
        self.state.validated_revision = self.state.document.project.revision
        has_errors = any(
            diagnostic.severity is DiagnosticSeverity.ERROR
            for diagnostic in self.state.diagnostics
        )
        binary_ready = (
            self.installation is not None
            and self.installation.binary is not None
            and self.installation.binary.is_file()
        )
        probe_ready = False
        if (
            binary_ready
            and self.execution_probe is not None
            and self.execution_probe.supports_x86
            and SUPPORTED_COMPONENT_TYPE_IDS.issubset(
                self.execution_probe.importable_type_ids
            )
        ):
            try:
                current_installation_fingerprint = installation_fingerprint(
                    self.installation
                )
            except OSError:
                # A binary may disappear or be replaced between is_file() and
                # hashing. Treat that as not runnable instead of crashing the
                # event loop.
                binary_ready = False
            else:
                probe_ready = (
                    self.execution_probe.binary_fingerprint
                    == current_installation_fingerprint
                )
        translation_ready = False
        translation_error = None
        if not has_errors and self.installation is not None:
            try:
                plan = translate_project(
                    self.state.document,
                    self.installation,
                    project_directory=self.project_path.expanduser()
                    .resolve()
                    .parent,
                )
                workload = plan.workload["parameters"]
                if (
                    workload["kind"] == "local"
                    and not Path(workload["path"]).is_file()
                ):
                    translation_error = (
                        f"Local workload does not exist: {workload['path']}"
                    )
                else:
                    translation_ready = True
            except (OSError, RuntimeError, ValueError) as error:
                translation_error = str(error)
        if has_errors:
            self.state.run_block_reason = (
                "Project has blocking validation errors"
            )
        elif self._catalog_source_stale:
            self.state.run_block_reason = (
                "gem5 Python sources changed; refresh the catalog"
            )
        elif not binary_ready:
            self.state.run_block_reason = "A built X86 gem5 binary is required"
        elif not probe_ready:
            self.state.run_block_reason = (
                "The selected gem5 binary has not passed capability probing"
            )
        elif not translation_ready:
            self.state.run_block_reason = (
                translation_error or "Project is not executable"
            )
        else:
            self.state.run_block_reason = None
        self.state.can_run = (
            not has_errors
            and not self._catalog_source_stale
            and probe_ready
            and translation_ready
        )

    def _resize(self, size: tuple[int, int]) -> None:
        width = max(MINIMUM_WINDOW_SIZE[0], int(size[0]))
        height = max(MINIMUM_WINDOW_SIZE[1], int(size[1]))
        new_size = (width, height)
        if self.surface.get_size() != new_size:
            self.surface = pygame.display.set_mode(new_size, pygame.RESIZABLE)
        self.window_layout = create_layout(new_size)
        self.toolbar.set_rect(self.window_layout.toolbar)
        self.palette.set_rect(self.window_layout.palette)
        self.canvas.set_rect(self.window_layout.canvas)
        self.canvas.ensure_components_visible(self.state)
        self.inspector.set_rect(self.window_layout.inspector)

    def new_project(self) -> None:
        self.canvas.cancel_interactions()
        self.inspector.cancel_focused()
        self.state.document = ProjectDocument()
        self.state.selected_type_id = None
        self.state.selected_component_id = None
        self.state.selected_connection_id = None
        self.state.status_message = "New project"
        self.refresh_validation()

    def save_project(self) -> None:
        try:
            save_project_document(self.project_path, self.state.document)
        except ProjectFileError as error:
            self.state.status_message = f"Could not save project: {error}"
        else:
            self.state.status_message = f"Saved {self.project_path.name}"

    def load_project(self) -> None:
        try:
            document = load_project_document(self.project_path)
        except ProjectFileError as error:
            detail = (
                error.diagnostics[0].message
                if error.diagnostics
                else str(error)
            )
            self.state.status_message = f"Could not load project: {detail}"
            return
        self.canvas.cancel_interactions()
        self.inspector.cancel_focused()
        self.state.document = document
        repaired = self.state.document.repair_layout()
        self.canvas.ensure_components_visible(self.state)
        self.state.selected_type_id = None
        self.state.selected_component_id = None
        self.state.selected_connection_id = None
        self.refresh_validation()
        notices: list[str] = []
        if repaired:
            notices.append(f"repaired {len(repaired)} layout entry(s)")
        if self.state.diagnostics:
            notices.append(
                f"{len(self.state.diagnostics)} validation issue(s)"
            )
        suffix = f" · {' · '.join(notices)}" if notices else ""
        self.state.status_message = f"Loaded {self.project_path.name}{suffix}"

    def build_gem5(self) -> None:
        if self.installation is None:
            self.state.status_message = "No gem5 repository is configured"
            return
        if self.state.catalog_busy or (
            self._catalog_thread is not None
            and self._catalog_thread.is_alive()
        ):
            self.state.status_message = (
                "Wait for catalog probing to finish before building"
            )
            return
        try:
            handle = self.controller.start_build(
                BuildRequest(self.installation)
            )
        except ExecutionControllerError as error:
            self.state.status_message = f"Could not start build: {error}"
            return
        self._handled_terminal_job_id = None
        self.state.job_kind = "build"
        self.state.job_state = "starting"
        self.state.job_output_path = str(handle.output_dir)
        self.state.status_message = "Building build/X86/gem5.opt"

    def run_project(self) -> None:
        self.refresh_validation()
        if self.installation is None or self.installation.binary is None:
            self.state.status_message = "A built X86 gem5 binary is required"
            return
        if self.state.catalog_busy or (
            self._catalog_thread is not None
            and self._catalog_thread.is_alive()
        ):
            self.state.status_message = (
                "Wait for catalog probing to finish before running"
            )
            return
        if self.execution_probe is None:
            self.refresh_catalog()
            self.state.status_message = (
                "Probing the selected gem5 binary; Run will enable when ready"
            )
            return
        if not self.state.can_run:
            errors = sum(
                item.severity is DiagnosticSeverity.ERROR
                for item in self.state.diagnostics
            )
            self.state.status_message = (
                f"Project has {errors} blocking validation error(s)"
                if errors
                else self.state.run_block_reason or "Project is not executable"
            )
            return
        try:
            current_source_fingerprint = fingerprint_source_tree(
                self.installation.repo_root
            )
        except (OSError, ValueError) as error:
            self._catalog_source_stale = True
            self.refresh_validation()
            self.state.status_message = (
                f"Could not verify gem5 sources: {error}; refresh the catalog"
            )
            return
        if (
            self.catalog is None
            or current_source_fingerprint != self.catalog.source_fingerprint
        ):
            self._catalog_source_stale = True
            self.refresh_validation()
            self.state.status_message = (
                "gem5 Python sources changed; refresh the catalog before "
                "running"
            )
            return
        try:
            if not self.execution_probe.supports_x86:
                self.state.status_message = (
                    "The selected gem5 binary does not support X86"
                )
                return
            handle = self.controller.start_run(
                RunRequest(
                    self.state.document,
                    self.installation,
                    project_name=self.project_path.stem,
                    probe=self.execution_probe,
                    project_path=self.project_path,
                    catalog_fingerprint=(
                        self.catalog.source_fingerprint
                        if self.catalog is not None
                        else None
                    ),
                )
            )
        except (ExecutionControllerError, Gem5ProbeError, ValueError) as error:
            self.state.status_message = f"Could not run project: {error}"
            return
        self._handled_terminal_job_id = None
        self.state.job_kind = "run"
        self.state.job_state = "starting"
        self.state.job_output_path = str(handle.output_dir)
        self.state.status_message = "Starting gem5 simulation"

    def stop_job(self) -> None:
        if self.controller.request_stop():
            self.state.status_message = "Stopping active job"

    def _poll_job(self) -> None:
        snapshot = self.controller.poll()
        self.state.job_state = snapshot.state.value
        self.state.job_kind = snapshot.kind.value if snapshot.kind else None
        self.state.job_output_path = (
            str(snapshot.output_dir) if snapshot.output_dir else None
        )
        self.state.job_log_lines = snapshot.recent_output.splitlines()[-200:]
        if not snapshot.state.terminal or snapshot.job_id is None:
            return
        if self._handled_terminal_job_id == snapshot.job_id:
            return
        self._handled_terminal_job_id = snapshot.job_id
        if snapshot.state is JobState.SUCCEEDED:
            self.state.status_message = f"{snapshot.kind.value.title()} succeeded · {snapshot.output_dir}"
            if (
                snapshot.kind.value == "build"
                and self.installation is not None
            ):
                try:
                    self.installation = discover_gem5_installation(
                        repo_root=self.installation.repo_root
                    )
                except Gem5DiscoveryError as error:
                    self.state.status_message = f"Build finished: {error}"
                else:
                    self.execution_probe = None
                    self.state.gem5_binary = (
                        str(self.installation.binary)
                        if self.installation.binary
                        else None
                    )
                    self.refresh_catalog()
        elif snapshot.state is JobState.CANCELLED:
            self.state.status_message = (
                "Job cancelled; partial output retained"
            )
        else:
            self.state.status_message = (
                snapshot.error
                or f"{snapshot.kind.value.title()} failed with "
                f"status {snapshot.return_code}"
            )
        self.refresh_validation()

    def _handle_shortcut(self, event: pygame.event.Event) -> bool:
        if event.type == pygame.KEYDOWN and event.key == pygame.K_F5:
            if not self.inspector.commit_focused():
                return True
            self.run_project()
            return True
        if event.type != pygame.KEYDOWN or not getattr(event, "mod", 0) & (
            pygame.KMOD_CTRL | pygame.KMOD_META
        ):
            return False
        if event.key == pygame.K_n:
            self.new_project()
        elif event.key == pygame.K_s:
            if not self.inspector.commit_focused():
                return True
            self.save_project()
        elif event.key in (pygame.K_o, pygame.K_l):
            self.load_project()
        elif event.key == pygame.K_b:
            self.build_gem5()
        elif event.key == pygame.K_r:
            self.refresh_catalog()
        else:
            return False
        return True

    def _event_panels(self) -> tuple[Panel, ...]:
        focused: Panel | None = None
        if self.inspector.has_focus:
            focused = self.inspector
        elif self.palette.has_focus:
            focused = self.palette
        if focused is None:
            return self.panels
        return (
            focused,
            *(panel for panel in self.panels if panel is not focused),
        )

    def _handle_event(self, event: pygame.event.Event) -> None:
        if event.type == pygame.QUIT:
            self.running = False
            return
        if event.type == pygame.VIDEORESIZE:
            self._resize(event.size)
            return
        if event.type in (pygame.WINDOWRESIZED, pygame.WINDOWSIZECHANGED):
            self._resize((event.x, event.y))
            return
        if event.type == pygame.WINDOWFOCUSLOST:
            self.canvas.cancel_interactions()
            self.toolbar.cancel_interactions()
            self.palette.cancel_interactions()
            return
        if self._handle_shortcut(event):
            return
        for panel in self._event_panels():
            if panel.handle_event(event, self.state):
                break

    def _update(self, dt: float) -> None:
        self._consume_catalog_updates()
        if (
            self.state.validated_revision
            != self.state.document.project.revision
        ):
            self.refresh_validation()
        self._job_poll_elapsed += dt
        if self._job_poll_elapsed >= 0.2:
            self._job_poll_elapsed = 0.0
            self._poll_job()
        for panel in self.panels:
            panel.update(dt, self.state)

    def _draw_status_bar(self) -> None:
        rect = self.window_layout.status_bar
        if rect.width <= 0 or rect.height <= 0:
            return
        pygame.draw.rect(self.surface, self.theme.panel_background, rect)
        pygame.draw.line(
            self.surface,
            self.theme.panel_border,
            rect.topleft,
            rect.topright,
        )
        font = get_font(13)
        message = ellipsize(
            self.state.status_message, font, max(0, rect.width - 24)
        )
        draw_text(
            self.surface,
            message,
            (rect.x + 12, rect.centery),
            color=self.theme.muted_text,
            size=13,
            anchor="midleft",
        )

    def draw(self) -> None:
        self.surface.fill(self.theme.window_background)
        for panel in self.panels:
            panel.draw(self.surface, self.state)
        self._draw_status_bar()

    def run(self) -> None:
        self.running = True
        try:
            while self.running:
                dt = self.clock.tick(TARGET_FPS) / 1000.0
                for event in pygame.event.get():
                    self._handle_event(event)
                self._update(dt)
                self.draw()
                pygame.display.flip()
        finally:
            self._catalog_cancel.set()
            if self._catalog_thread is not None:
                self._catalog_thread.join(3)
            snapshot = self.controller.poll()
            if (
                not snapshot.state.terminal
                and snapshot.state is not JobState.IDLE
            ):
                self.controller.request_stop()
                self.controller.wait(5)
            pygame.quit()
