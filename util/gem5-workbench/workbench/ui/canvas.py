"""Interactive node canvas for assembling a gem5 configuration."""

import pygame

from workbench.state import CanvasNode, WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import draw_text, ellipsize, get_font


NODE_SIZE = (150, 64)
GRID_SIZE = 24


class Canvas(Panel):
    def __init__(self, rect: pygame.Rect, theme: Theme) -> None:
        super().__init__(rect, theme)
        self._dragging_node_id: str | None = None
        self._drag_offset = pygame.Vector2()

    def _node_rect(self, node: CanvasNode) -> pygame.Rect:
        return pygame.Rect(
            self.rect.x + round(node.position.x),
            self.rect.y + round(node.position.y),
            *NODE_SIZE,
        )

    def _node_at(
        self, point: tuple[int, int], state: WorkbenchState
    ) -> CanvasNode | None:
        for node in reversed(state.nodes):
            if self._node_rect(node).collidepoint(point):
                return node
        return None

    @staticmethod
    def _node_by_id(node_id: str, state: WorkbenchState) -> CanvasNode | None:
        return next((node for node in state.nodes if node.id == node_id), None)

    def _clamp_position(self, position: pygame.Vector2) -> pygame.Vector2:
        return pygame.Vector2(
            max(0, min(position.x, max(0, self.rect.width - NODE_SIZE[0]))),
            max(0, min(position.y, max(0, self.rect.height - NODE_SIZE[1]))),
        )

    def constrain_nodes(self, state: WorkbenchState) -> None:
        """Move nodes into the visible area after loading or resizing."""

        for node in state.nodes:
            node.position = self._clamp_position(node.position)

    def _add_node(
        self,
        kind: str,
        point: tuple[int, int],
        state: WorkbenchState,
    ) -> None:
        local_position = pygame.Vector2(
            point[0] - self.rect.x - NODE_SIZE[0] / 2,
            point[1] - self.rect.y - NODE_SIZE[1] / 2,
        )
        node = CanvasNode(kind, self._clamp_position(local_position))
        state.nodes.append(node)
        state.selected_node_id = node.id
        state.status_message = f"Added {kind}"

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        if event.type == pygame.KEYDOWN:
            if event.key == pygame.K_ESCAPE:
                state.selected_component = None
                self._dragging_node_id = None
                state.status_message = "Selection tool cleared"
                return True
            if event.key in (pygame.K_DELETE, pygame.K_BACKSPACE):
                selected = self._node_by_id(state.selected_node_id or "", state)
                if selected is not None:
                    state.nodes.remove(selected)
                    state.selected_node_id = None
                    state.status_message = f"Removed {selected.kind}"
                    return True

        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if not self.rect.collidepoint(event.pos):
                return False
            selected = self._node_at(event.pos, state)
            if selected is not None:
                state.selected_node_id = selected.id
                state.selected_component = None
                self._dragging_node_id = selected.id
                node_rect = self._node_rect(selected)
                self._drag_offset = pygame.Vector2(
                    event.pos[0] - node_rect.x, event.pos[1] - node_rect.y
                )
                state.status_message = f"Selected {selected.kind}"
            elif state.selected_component:
                self._add_node(state.selected_component, event.pos, state)
                state.selected_component = None
            else:
                state.selected_node_id = None
                state.status_message = "Ready"
            return True

        if event.type == pygame.MOUSEMOTION and self._dragging_node_id is not None:
            node = self._node_by_id(self._dragging_node_id, state)
            if node is not None:
                position = pygame.Vector2(
                    event.pos[0] - self.rect.x,
                    event.pos[1] - self.rect.y,
                ) - self._drag_offset
                node.position = self._clamp_position(position)
                return True
            self._dragging_node_id = None

        if event.type == pygame.MOUSEBUTTONUP and event.button == 1:
            if self._dragging_node_id is not None:
                self._dragging_node_id = None
                return True
        return False

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        if self.rect.width <= 0 or self.rect.height <= 0:
            return
        pygame.draw.rect(surface, self.theme.canvas_background, self.rect)
        previous_clip = surface.get_clip()
        surface.set_clip(self.rect)
        try:
            for x in range(self.rect.left, self.rect.right, GRID_SIZE):
                pygame.draw.line(
                    surface, self.theme.grid, (x, self.rect.top), (x, self.rect.bottom)
                )
            for y in range(self.rect.top, self.rect.bottom, GRID_SIZE):
                pygame.draw.line(
                    surface, self.theme.grid, (self.rect.left, y), (self.rect.right, y)
                )

            label_font = get_font(16, bold=True)
            for node in state.nodes:
                node_rect = self._node_rect(node)
                pygame.draw.rect(
                    surface, self.theme.node_background, node_rect, border_radius=8
                )
                selected = node.id == state.selected_node_id
                border = self.theme.accent if selected else self.theme.node_border
                pygame.draw.rect(
                    surface,
                    border,
                    node_rect,
                    width=2 if selected else 1,
                    border_radius=8,
                )
                label = node.kind.replace("_", " ").title()
                draw_text(
                    surface,
                    ellipsize(label, label_font, node_rect.width - 24),
                    node_rect.center,
                    color=self.theme.text,
                    size=16,
                    bold=True,
                    anchor="center",
                )

            if not state.nodes:
                draw_text(
                    surface,
                    "Choose a component, then place it here",
                    self.rect.center,
                    color=self.theme.muted_text,
                    size=17,
                    anchor="center",
                )
        finally:
            surface.set_clip(previous_clip)
