"""Component palette used to choose nodes for the canvas."""

from dataclasses import dataclass

import pygame

from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import draw_panel, draw_text, ellipsize, get_font


@dataclass(frozen=True, slots=True)
class PaletteItem:
    kind: str
    label: str
    description: str


COMPONENTS = (
    PaletteItem("board", "Board", "System platform"),
    PaletteItem("processor", "Processor", "CPU cores"),
    PaletteItem("cache", "Cache hierarchy", "Memory-side caches"),
    PaletteItem("memory", "Memory", "Physical memory"),
    PaletteItem("workload", "Workload", "Program or resource"),
)


class Palette(Panel):
    HEADER_HEIGHT = 56
    ITEM_HEIGHT = 58
    PADDING = 12

    def __init__(self, rect: pygame.Rect, theme: Theme) -> None:
        super().__init__(rect, theme)
        self._scroll = 0

    def _item_rect(self, index: int) -> pygame.Rect:
        return pygame.Rect(
            self.rect.x + self.PADDING,
            self.rect.y
            + self.HEADER_HEIGHT
            + index * self.ITEM_HEIGHT
            - self._scroll,
            max(0, self.rect.width - self.PADDING * 2),
            self.ITEM_HEIGHT - 8,
        )

    def _max_scroll(self) -> int:
        content_height = len(COMPONENTS) * self.ITEM_HEIGHT
        return max(0, content_height - max(0, self.rect.height - self.HEADER_HEIGHT))

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        if event.type == pygame.MOUSEWHEEL:
            if self.rect.collidepoint(pygame.mouse.get_pos()):
                self._scroll = max(
                    0, min(self._max_scroll(), self._scroll - event.y * 30)
                )
                return True
        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if not self.rect.collidepoint(event.pos):
                return False
            for index, item in enumerate(COMPONENTS):
                if self._item_rect(index).collidepoint(event.pos):
                    state.selected_component = item.kind
                    state.selected_node_id = None
                    state.status_message = (
                        f"{item.label} selected — click the canvas to place it"
                    )
                    return True
            return True
        return False

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        draw_panel(surface, self.rect, self.theme)
        if self.rect.width < 1 or self.rect.height < 1:
            return

        previous_clip = surface.get_clip()
        surface.set_clip(self.rect)
        try:
            draw_text(
                surface,
                "Components",
                (self.rect.x + self.PADDING, self.rect.y + 14),
                color=self.theme.text,
                size=18,
                bold=True,
            )
            mouse_position = pygame.mouse.get_pos()
            label_font = get_font(15, bold=True)
            description_font = get_font(12)
            for index, item in enumerate(COMPONENTS):
                item_rect = self._item_rect(index)
                if not item_rect.colliderect(self.rect):
                    continue
                selected = state.selected_component == item.kind
                hovered = item_rect.collidepoint(mouse_position)
                fill = self.theme.button_hover if hovered else self.theme.button
                if selected:
                    fill = self.theme.node_background
                pygame.draw.rect(surface, fill, item_rect, border_radius=6)
                if selected:
                    pygame.draw.rect(
                        surface,
                        self.theme.accent,
                        item_rect,
                        width=2,
                        border_radius=6,
                    )
                max_text_width = max(0, item_rect.width - 20)
                draw_text(
                    surface,
                    ellipsize(item.label, label_font, max_text_width),
                    (item_rect.x + 10, item_rect.y + 7),
                    color=self.theme.text,
                    size=15,
                    bold=True,
                )
                draw_text(
                    surface,
                    ellipsize(item.description, description_font, max_text_width),
                    (item_rect.x + 10, item_rect.y + 28),
                    color=self.theme.muted_text,
                    size=12,
                )
        finally:
            surface.set_clip(previous_clip)
