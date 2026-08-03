"""Main pygame application for the gem5 Workbench."""

import json
import math
from pathlib import Path
from typing import Any

import pygame

from workbench.constants import (
    INITIAL_WINDOW_SIZE,
    MINIMUM_WINDOW_SIZE,
    TARGET_FPS,
    WIN_TITLE,
)
from workbench.state import CanvasNode, WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.canvas import Canvas
from workbench.ui.inspector import Inspector
from workbench.ui.layout import create_layout
from workbench.ui.palette import Palette
from workbench.ui.theme import Theme
from workbench.ui.toolbar import Toolbar
from workbench.ui.widgets import draw_text, ellipsize, get_font


PROJECT_PATH = Path(__file__).resolve().parent.parent / "projects" / "current.g5proj"


class WorkbenchApp:
    """Own the pygame lifecycle, shared state, and top-level panels."""

    def __init__(self) -> None:
        pygame.init()
        pygame.display.set_caption(WIN_TITLE)
        self.surface = pygame.display.set_mode(INITIAL_WINDOW_SIZE, pygame.RESIZABLE)
        self.clock = pygame.time.Clock()
        self.theme = Theme()
        self.state = WorkbenchState()
        self.layout = create_layout(self.surface.get_size())

        self.toolbar = Toolbar(
            self.layout.toolbar,
            self.theme,
            on_new=self.new_project,
            on_save=self.save_project,
            on_load=self.load_project,
        )
        self.palette = Palette(self.layout.palette, self.theme)
        self.canvas = Canvas(self.layout.canvas, self.theme)
        self.inspector = Inspector(self.layout.inspector, self.theme)
        self.panels: tuple[Panel, ...] = (
            self.toolbar,
            self.palette,
            self.canvas,
            self.inspector,
        )
        self.running = False

    def _resize(self, size: tuple[int, int]) -> None:
        width = max(MINIMUM_WINDOW_SIZE[0], int(size[0]))
        height = max(MINIMUM_WINDOW_SIZE[1], int(size[1]))
        new_size = (width, height)
        if self.surface.get_size() != new_size:
            self.surface = pygame.display.set_mode(new_size, pygame.RESIZABLE)
        self.layout = create_layout(new_size)
        self.toolbar.set_rect(self.layout.toolbar)
        self.palette.set_rect(self.layout.palette)
        self.canvas.set_rect(self.layout.canvas)
        self.canvas.constrain_nodes(self.state)
        self.inspector.set_rect(self.layout.inspector)

    def new_project(self) -> None:
        self.state.nodes.clear()
        self.state.selected_component = None
        self.state.selected_node_id = None
        self.state.status_message = "New project"

    def save_project(self) -> None:
        document = {
            "format": "gem5-workbench",
            "version": 1,
            "nodes": [
                {
                    "id": node.id,
                    "kind": node.kind,
                    "position": [node.position.x, node.position.y],
                }
                for node in self.state.nodes
            ],
        }
        try:
            PROJECT_PATH.parent.mkdir(parents=True, exist_ok=True)
            temporary_path = PROJECT_PATH.with_suffix(".g5proj.tmp")
            temporary_path.write_text(
                json.dumps(document, indent=2) + "\n", encoding="utf-8"
            )
            temporary_path.replace(PROJECT_PATH)
        except OSError as error:
            self.state.status_message = f"Could not save project: {error}"
        else:
            self.state.status_message = f"Saved {PROJECT_PATH.name}"

    @staticmethod
    def _nodes_from_document(document: Any) -> list[CanvasNode]:
        if not isinstance(document, dict) or document.get("format") != "gem5-workbench":
            raise ValueError("not a gem5 Workbench project")
        if document.get("version") != 1:
            raise ValueError("unsupported project version")
        raw_nodes = document.get("nodes")
        if not isinstance(raw_nodes, list):
            raise ValueError("project nodes must be a list")

        nodes: list[CanvasNode] = []
        node_ids: set[str] = set()
        for raw_node in raw_nodes:
            if not isinstance(raw_node, dict):
                raise ValueError("each node must be an object")
            kind = raw_node.get("kind")
            node_id = raw_node.get("id")
            position = raw_node.get("position")
            if not isinstance(kind, str) or not kind.strip():
                raise ValueError("node kind must be a non-empty string")
            valid_position = (
                isinstance(position, list)
                and len(position) == 2
                and all(
                    not isinstance(value, bool)
                    and isinstance(value, (int, float))
                    for value in position
                )
            )
            try:
                valid_position = valid_position and all(
                    math.isfinite(value) for value in position
                )
            except OverflowError:
                valid_position = False
            if not valid_position:
                raise ValueError("node position must contain two finite numbers")
            if node_id is not None and (
                not isinstance(node_id, str) or not node_id.strip()
            ):
                raise ValueError("node id must be a non-empty string")
            node = CanvasNode(kind.strip(), pygame.Vector2(*position))
            if node_id is not None:
                node.id = node_id.strip()
            if node.id in node_ids:
                raise ValueError("node ids must be unique")
            node_ids.add(node.id)
            nodes.append(node)
        return nodes

    def load_project(self) -> None:
        try:
            document = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))
            nodes = self._nodes_from_document(document)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            self.state.status_message = f"Could not load project: {error}"
            return
        self.state.nodes = nodes
        self.canvas.constrain_nodes(self.state)
        self.state.selected_component = None
        self.state.selected_node_id = None
        self.state.status_message = f"Loaded {PROJECT_PATH.name}"

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
        for panel in self.panels:
            if panel.handle_event(event, self.state):
                break

    def _draw_status_bar(self) -> None:
        rect = self.layout.status_bar
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
