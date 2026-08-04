"""Pygame canvas adapter for the rendering-independent project document."""

import pygame

from workbench.layout import ComponentLayout, Position
from workbench.model import Component, ComponentId
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import draw_text, ellipsize, get_font


NODE_SIZE = (150, 64)
GRID_SIZE = 24


class Canvas(Panel):
    def __init__(self, rect: pygame.Rect, theme: Theme) -> None:
        super().__init__(rect, theme)
        self._dragging_component_id: ComponentId | None = None
        self._drag_offset = pygame.Vector2()

    @staticmethod
    def _layout_for(
        component_id: ComponentId, state: WorkbenchState
    ) -> ComponentLayout | None:
        return state.document.layout.components.get(component_id)

    def _component_rect(
        self, component: Component, state: WorkbenchState
    ) -> pygame.Rect:
        component_layout = self._layout_for(component.id, state)
        position = component_layout.position if component_layout else Position()
        return pygame.Rect(
            self.rect.x + round(position.x),
            self.rect.y + round(position.y),
            *NODE_SIZE,
        )

    def _component_at(
        self, point: tuple[int, int], state: WorkbenchState
    ) -> Component | None:
        for component in reversed(tuple(state.document.project.components.values())):
            if self._component_rect(component, state).collidepoint(point):
                return component
        return None

    def _clamp_position(self, position: Position) -> Position:
        return Position(
            max(0.0, min(position.x, max(0, self.rect.width - NODE_SIZE[0]))),
            max(0.0, min(position.y, max(0, self.rect.height - NODE_SIZE[1]))),
        )

    def constrain_components(self, state: WorkbenchState) -> None:
        for component_layout in state.document.layout.components.values():
            component_layout.position = self._clamp_position(
                component_layout.position
            )

    def _add_component(
        self,
        type_id: str,
        point: tuple[int, int],
        state: WorkbenchState,
    ) -> None:
        position = self._clamp_position(
            Position(
                point[0] - self.rect.x - NODE_SIZE[0] / 2,
                point[1] - self.rect.y - NODE_SIZE[1] / 2,
            )
        )
        component = state.document.add_component(type_id, position)
        state.selected_component_id = component.id
        state.selected_connection_id = None
        state.diagnostics.clear()
        state.status_message = f"Added {type_id}"

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        if event.type == pygame.KEYDOWN:
            if event.key == pygame.K_ESCAPE:
                state.selected_type_id = None
                self._dragging_component_id = None
                state.status_message = "Selection tool cleared"
                return True
            if event.key in (pygame.K_DELETE, pygame.K_BACKSPACE):
                if state.selected_connection_id is not None:
                    state.document.project.remove_connection(
                        state.selected_connection_id
                    )
                    state.selected_connection_id = None
                    state.diagnostics.clear()
                    state.status_message = "Removed connection"
                    return True
                component_id = state.selected_component_id
                if component_id in state.document.project.components:
                    component = state.document.remove_component(component_id)
                    state.selected_component_id = None
                    state.diagnostics.clear()
                    state.status_message = f"Removed {component.type_id}"
                    return True

        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if not self.rect.collidepoint(event.pos):
                return False
            selected = self._component_at(event.pos, state)
            if selected is not None:
                state.selected_component_id = selected.id
                state.selected_connection_id = None
                state.selected_type_id = None
                self._dragging_component_id = selected.id
                component_rect = self._component_rect(selected, state)
                self._drag_offset = pygame.Vector2(
                    event.pos[0] - component_rect.x,
                    event.pos[1] - component_rect.y,
                )
                state.status_message = f"Selected {selected.type_id}"
            elif state.selected_type_id:
                self._add_component(state.selected_type_id, event.pos, state)
                state.selected_type_id = None
            else:
                state.selected_component_id = None
                state.selected_connection_id = None
                state.status_message = "Ready"
            return True

        if (
            event.type == pygame.MOUSEMOTION
            and self._dragging_component_id is not None
        ):
            component_layout = self._layout_for(
                self._dragging_component_id, state
            )
            if component_layout is not None:
                component_layout.position = self._clamp_position(
                    Position(
                        event.pos[0] - self.rect.x - self._drag_offset.x,
                        event.pos[1] - self.rect.y - self._drag_offset.y,
                    )
                )
                return True
            self._dragging_component_id = None

        if event.type == pygame.MOUSEBUTTONUP and event.button == 1:
            if self._dragging_component_id is not None:
                self._dragging_component_id = None
                return True
        return False

    def _draw_connections(
        self,
        surface: pygame.Surface,
        state: WorkbenchState,
    ) -> None:
        components = state.document.project.components
        for connection in state.document.project.connections.values():
            source = components.get(connection.source.component_id)
            target = components.get(connection.target.component_id)
            if source is None or target is None:
                continue
            source_layout = self._layout_for(source.id, state)
            target_layout = self._layout_for(target.id, state)
            if source_layout is None or target_layout is None:
                continue
            color = (
                self.theme.accent
                if connection.id == state.selected_connection_id
                else self.theme.muted_text
            )
            pygame.draw.line(
                surface,
                color,
                self._component_rect(source, state).center,
                self._component_rect(target, state).center,
                width=2,
            )

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
            self._draw_connections(surface, state)

            label_font = get_font(16, bold=True)
            for component in state.document.project.components.values():
                component_rect = self._component_rect(component, state)
                pygame.draw.rect(
                    surface,
                    self.theme.node_background,
                    component_rect,
                    border_radius=8,
                )
                selected = component.id == state.selected_component_id
                border = self.theme.accent if selected else self.theme.node_border
                pygame.draw.rect(
                    surface,
                    border,
                    component_rect,
                    width=2 if selected else 1,
                    border_radius=8,
                )
                label = component.type_id.replace("_", " ").title()
                draw_text(
                    surface,
                    ellipsize(label, label_font, component_rect.width - 24),
                    component_rect.center,
                    color=self.theme.text,
                    size=16,
                    bold=True,
                    anchor="center",
                )

            if not state.document.project.components:
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
