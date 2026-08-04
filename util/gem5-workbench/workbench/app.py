"""Main pygame application for the gem5 Workbench."""

from pathlib import Path

import pygame

from workbench.constants import (
    INITIAL_WINDOW_SIZE,
    MINIMUM_WINDOW_SIZE,
    TARGET_FPS,
    WIN_TITLE,
)
from workbench.document import ProjectDocument
from workbench.persistence import (
    ProjectFileError,
    load_project_document,
    save_project_document,
)
from workbench.registry import create_builtin_registry
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.canvas import Canvas
from workbench.ui.inspector import Inspector
from workbench.ui.palette import Palette
from workbench.ui.theme import Theme
from workbench.ui.toolbar import Toolbar
from workbench.ui.widgets import draw_text, ellipsize, get_font
from workbench.ui.window_layout import create_layout
from workbench.validation import validate_project


PROJECT_PATH = Path(__file__).resolve().parent.parent / "projects" / "current.g5proj"


class WorkbenchApp:
    """Own the pygame lifecycle, project document, registry, and UI panels."""

    def __init__(self) -> None:
        pygame.init()
        pygame.display.set_caption(WIN_TITLE)
        self.surface = pygame.display.set_mode(INITIAL_WINDOW_SIZE, pygame.RESIZABLE)
        self.clock = pygame.time.Clock()
        self.theme = Theme()
        self.registry = create_builtin_registry()
        self.state = WorkbenchState()
        self.window_layout = create_layout(self.surface.get_size())

        self.toolbar = Toolbar(
            self.window_layout.toolbar,
            self.theme,
            on_new=self.new_project,
            on_save=self.save_project,
            on_load=self.load_project,
        )
        self.palette = Palette(
            self.window_layout.palette, self.theme, self.registry
        )
        self.canvas = Canvas(self.window_layout.canvas, self.theme)
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

    def refresh_validation(self) -> None:
        document = self.state.document
        self.state.diagnostics = validate_project(document, self.registry)

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
        self.inspector.set_rect(self.window_layout.inspector)

    def new_project(self) -> None:
        self.state.document = ProjectDocument()
        self.state.selected_type_id = None
        self.state.selected_component_id = None
        self.state.selected_connection_id = None
        self.state.diagnostics.clear()
        self.state.status_message = "New project"

    def save_project(self) -> None:
        try:
            save_project_document(PROJECT_PATH, self.state.document)
        except ProjectFileError as error:
            self.state.status_message = f"Could not save project: {error}"
        else:
            self.state.status_message = f"Saved {PROJECT_PATH.name}"

    def load_project(self) -> None:
        try:
            document = load_project_document(PROJECT_PATH)
        except ProjectFileError as error:
            detail = (
                error.diagnostics[0].message
                if error.diagnostics
                else str(error)
            )
            self.state.status_message = f"Could not load project: {detail}"
            return
        self.state.document = document
        self.state.selected_type_id = None
        self.state.selected_component_id = None
        self.state.selected_connection_id = None
        self.refresh_validation()
        suffix = (
            f" · {len(self.state.diagnostics)} validation issue(s)"
            if self.state.diagnostics
            else ""
        )
        self.state.status_message = f"Loaded {PROJECT_PATH.name}{suffix}"

    def _handle_shortcut(self, event: pygame.event.Event) -> bool:
        if event.type != pygame.KEYDOWN or not getattr(event, "mod", 0) & pygame.KMOD_CTRL:
            return False
        if event.key == pygame.K_n:
            self.new_project()
        elif event.key == pygame.K_s:
            self.save_project()
        elif event.key in (pygame.K_o, pygame.K_l):
            self.load_project()
        else:
            return False
        return True

    def _handle_event(self, event: pygame.event.Event) -> None:
        if event.type == pygame.QUIT:
            self.running = False
            return
        if event.type == pygame.VIDEORESIZE:
            self._resize(event.size)
            return
        if self._handle_shortcut(event):
            return
        consumed = False
        for panel in self.panels:
            if panel.handle_event(event, self.state):
                consumed = True
                break
        if consumed and event.type in (pygame.KEYDOWN, pygame.MOUSEBUTTONDOWN):
            self.refresh_validation()

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
        message = ellipsize(self.state.status_message, font, max(0, rect.width - 24))
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
                for panel in self.panels:
                    panel.update(dt, self.state)
                self.draw()
                pygame.display.flip()
        finally:
            pygame.quit()
