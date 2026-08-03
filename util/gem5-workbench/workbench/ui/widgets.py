"""Small pygame widgets shared by the workbench panels."""

from collections.abc import Callable
from dataclasses import dataclass

import pygame

from workbench.ui.theme import Color, Theme


_FONT_CACHE: dict[tuple[int, bool], pygame.font.Font] = {}
_TEXT_ANCHORS = {
    "topleft",
    "topright",
    "bottomleft",
    "bottomright",
    "midtop",
    "midleft",
    "midbottom",
    "midright",
    "center",
}


def get_font(size: int, *, bold: bool = False) -> pygame.font.Font:
    """Return a cached system font, initializing pygame's font module lazily."""

    if not pygame.font.get_init():
        pygame.font.init()
    key = (max(1, int(size)), bold)
    if key not in _FONT_CACHE:
        _FONT_CACHE[key] = pygame.font.SysFont("sans", key[0], bold=bold)
    return _FONT_CACHE[key]


def draw_text(
    surface: pygame.Surface,
    text: object,
    position: tuple[int, int],
    *,
    color: Color,
    size: int = 16,
    bold: bool = False,
    anchor: str = "topleft",
) -> pygame.Rect:
    """Draw text and return its destination rectangle."""

    rendered = get_font(size, bold=bold).render(str(text), True, color)
    rect = rendered.get_rect()
    if anchor not in _TEXT_ANCHORS:
        raise ValueError(f"unknown text anchor: {anchor}")
    setattr(rect, anchor, position)
    surface.blit(rendered, rect)
    return rect


def draw_panel(
    surface: pygame.Surface,
    rect: pygame.Rect,
    theme: Theme,
    *,
    border_width: int = 1,
) -> None:
    """Draw a standard panel background and inset border."""

    if rect.width <= 0 or rect.height <= 0:
        return
    pygame.draw.rect(surface, theme.panel_background, rect)
    if border_width:
        pygame.draw.rect(surface, theme.panel_border, rect, border_width)


def ellipsize(text: object, font: pygame.font.Font, max_width: int) -> str:
    """Shorten text to fit a pixel width, adding an ellipsis when needed."""

    value = str(text)
    if max_width <= 0:
        return ""
    if font.size(value)[0] <= max_width:
        return value
    suffix = "…"
    if font.size(suffix)[0] > max_width:
        return ""
    low, high = 0, len(value)
    while low < high:
        middle = (low + high + 1) // 2
        if font.size(value[:middle] + suffix)[0] <= max_width:
            low = middle
        else:
            high = middle - 1
    return value[:low] + suffix


@dataclass(slots=True)
class Button:
    """A compact push button with hover, pressed, and disabled states."""

    label: str
    rect: pygame.Rect
    on_click: Callable[[], None] | None = None
    enabled: bool = True
    _pressed: bool = False

    def handle_event(self, event: pygame.event.Event) -> bool:
        if not self.enabled:
            self._pressed = False
            return False
        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if self.rect.collidepoint(event.pos):
                self._pressed = True
                return True
        elif event.type == pygame.MOUSEBUTTONUP and event.button == 1:
            was_pressed = self._pressed
            self._pressed = False
            if was_pressed:
                if self.rect.collidepoint(event.pos) and self.on_click:
                    self.on_click()
                return True
        return False

    def draw(
        self,
        surface: pygame.Surface,
        theme: Theme,
        mouse_position: tuple[int, int],
    ) -> None:
        hovered = self.enabled and self.rect.collidepoint(mouse_position)
        color = theme.button_hover if hovered else theme.button
        text_color = theme.text if self.enabled else theme.muted_text
        pygame.draw.rect(surface, color, self.rect, border_radius=5)
        if self._pressed:
            pygame.draw.rect(
                surface, theme.accent, self.rect, width=1, border_radius=5
            )
        draw_text(
            surface,
            self.label,
            self.rect.center,
            color=text_color,
            size=15,
            anchor="center",
        )
