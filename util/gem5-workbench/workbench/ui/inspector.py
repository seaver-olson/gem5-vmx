"""Read-only properties panel for the current project selection."""

import pygame

from workbench.registry import ComponentRegistry, create_builtin_registry
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import draw_panel, draw_text, ellipsize, get_font


class Inspector(Panel):
    PADDING = 16

    def __init__(
        self,
        rect: pygame.Rect,
        theme: Theme,
        registry: ComponentRegistry | None = None,
    ) -> None:
        super().__init__(rect, theme)
        self.registry = registry or create_builtin_registry()

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        return False

    def _row(
        self,
        surface: pygame.Surface,
        y: int,
        label: str,
        value: object,
    ) -> int:
        draw_text(
            surface,
            label,
            (self.rect.x + self.PADDING, y),
            color=self.theme.muted_text,
            size=13,
        )
        font = get_font(15)
        draw_text(
            surface,
            ellipsize(value, font, max(0, self.rect.width - self.PADDING * 2)),
            (self.rect.x + self.PADDING, y + 20),
            color=self.theme.text,
            size=15,
        )
        return y + 52

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        draw_panel(surface, self.rect, self.theme)
        if self.rect.width < 1 or self.rect.height < 1:
            return
        previous_clip = surface.get_clip()
        surface.set_clip(self.rect)
        try:
            draw_text(
                surface,
                "Inspector",
                (self.rect.x + self.PADDING, self.rect.y + 16),
                color=self.theme.text,
                size=18,
                bold=True,
            )
            y = self.rect.y + 58
            component = state.document.project.components.get(
                state.selected_component_id
            )
            if component is not None:
                definition = self.registry.get(component.type_id)
                display_name = (
                    definition.display_name
                    if definition
                    else f"Unknown ({component.type_id})"
                )
                y = self._row(surface, y, "Type", display_name)
                y = self._row(surface, y, "Component ID", component.id)
                component_layout = state.document.layout.components.get(component.id)
                if component_layout:
                    y = self._row(
                        surface, y, "Canvas X", round(component_layout.position.x)
                    )
                    self._row(
                        surface, y, "Canvas Y", round(component_layout.position.y)
                    )
                draw_text(
                    surface,
                    "Drag to reposition · Delete to remove",
                    (self.rect.x + self.PADDING, self.rect.bottom - 28),
                    color=self.theme.muted_text,
                    size=12,
                )
            elif state.selected_type_id:
                definition = self.registry.get(state.selected_type_id)
                self._row(
                    surface,
                    y,
                    "Component tool",
                    definition.display_name if definition else state.selected_type_id,
                )
                draw_text(
                    surface,
                    "Click the canvas to add this component.",
                    (self.rect.x + self.PADDING, y + 58),
                    color=self.theme.muted_text,
                    size=13,
                )
            else:
                draw_text(
                    surface,
                    "Select a component or canvas node",
                    (self.rect.x + self.PADDING, y),
                    color=self.theme.muted_text,
                    size=14,
                )
                draw_text(
                    surface,
                    "to view its properties.",
                    (self.rect.x + self.PADDING, y + 22),
                    color=self.theme.muted_text,
                    size=14,
                )
        finally:
            surface.set_clip(previous_clip)
