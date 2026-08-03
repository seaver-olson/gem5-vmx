"""Read-only properties panel for the current workbench selection."""

import pygame

from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import draw_panel, draw_text, ellipsize, get_font


class Inspector(Panel):
    PADDING = 16

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
        max_width = max(0, self.rect.width - self.PADDING * 2)
        draw_text(
            surface,
            ellipsize(value, font, max_width),
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
            node = next(
                (
                    node
                    for node in state.nodes
                    if node.id == state.selected_node_id
                ),
                None,
            )
            if node is not None:
                y = self._row(surface, y, "Type", node.kind.replace("_", " ").title())
                y = self._row(surface, y, "Component ID", node.id)
                y = self._row(surface, y, "Canvas X", round(node.position.x))
                self._row(surface, y, "Canvas Y", round(node.position.y))
                draw_text(
                    surface,
                    "Drag to reposition · Delete to remove",
                    (self.rect.x + self.PADDING, self.rect.bottom - 28),
                    color=self.theme.muted_text,
                    size=12,
                )
            elif state.selected_component:
                self._row(
                    surface,
                    y,
                    "Component tool",
                    state.selected_component.replace("_", " ").title(),
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
