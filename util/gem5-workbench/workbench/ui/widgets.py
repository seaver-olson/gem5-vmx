"""Small pygame widgets shared by the workbench panels."""

from collections.abc import Callable
from dataclasses import dataclass

import pygame
from workbench.ui.theme import (
    Color,
    Theme,
)

_FONT_CACHE: dict[tuple[int, bool], pygame.font.Font] = {}
_MAX_RENDER_TEXT = 4096
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


def display_text(value: object) -> str:
    """Return bounded single-line UTF-8 text accepted by pygame fonts."""

    text = str(value)
    truncated = len(text) > _MAX_RENDER_TEXT
    if truncated:
        text = text[:_MAX_RENDER_TEXT]
    text = text.encode("utf-8", errors="replace").decode("utf-8")
    text = "".join(
        (
            " "
            if character in "\t\r\n"
            else (
                "�"
                if ord(character) < 32 or ord(character) == 127
                else character
            )
        )
        for character in text
    )
    return text + ("…" if truncated else "")


def get_font(size: int, *, bold: bool = False) -> pygame.font.Font:
    """Return a cached system font, initializing pygame's font module lazily."""

    if not pygame.font.get_init():
        pygame.font.init()
    key = (max(1, int(size)), bold)
    cached = _FONT_CACHE.get(key)
    if cached is not None:
        try:
            cached.size("")
        except pygame.error:
            del _FONT_CACHE[key]
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

    rendered = get_font(size, bold=bold).render(
        display_text(text), True, color
    )
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

    value = display_text(text)
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

    def cancel_press(self) -> None:
        """Forget a pointer press that can no longer receive its release."""

        self._pressed = False

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


@dataclass(slots=True)
class TextInput:
    """A single-line text editor with explicit commit and revert behavior."""

    rect: pygame.Rect
    text: str = ""
    on_commit: Callable[[str], bool] | None = None
    enabled: bool = True
    focused: bool = False
    _committed_text: str = ""

    def __post_init__(self) -> None:
        self._committed_text = self.text

    def set_value(self, value: object) -> None:
        self.text = "" if value is None else str(value)
        self._committed_text = self.text

    def focus(self) -> None:
        if self.enabled:
            self.focused = True
            self._committed_text = self.text

    def commit(self) -> bool:
        if self.on_commit is not None and not self.on_commit(self.text):
            return False
        self._committed_text = self.text
        self.focused = False
        return True

    def revert(self) -> None:
        self.text = self._committed_text
        self.focused = False

    def handle_event(self, event: pygame.event.Event) -> bool:
        if not self.enabled:
            self.focused = False
            return False
        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if self.rect.collidepoint(event.pos):
                self.focus()
                return True
            if self.focused:
                return not self.commit()
        if event.type != pygame.KEYDOWN or not self.focused:
            return False
        if event.key in (pygame.K_RETURN, pygame.K_KP_ENTER):
            self.commit()
        elif event.key == pygame.K_ESCAPE:
            self.revert()
        elif event.key == pygame.K_BACKSPACE:
            self.text = self.text[:-1]
        elif event.key == pygame.K_DELETE:
            self.text = ""
        elif (
            getattr(event, "unicode", "")
            and event.unicode.isprintable()
            and not getattr(event, "mod", 0)
            & (pygame.KMOD_CTRL | pygame.KMOD_META)
        ):
            self.text += event.unicode
        return True

    def draw(self, surface: pygame.Surface, theme: Theme) -> None:
        fill = theme.button_hover if self.focused else theme.button
        pygame.draw.rect(surface, fill, self.rect, border_radius=4)
        pygame.draw.rect(
            surface,
            theme.accent if self.focused else theme.panel_border,
            self.rect,
            width=1,
            border_radius=4,
        )
        font = get_font(14)
        value = ellipsize(self.text, font, max(0, self.rect.width - 14))
        draw_text(
            surface,
            value,
            (self.rect.x + 7, self.rect.centery),
            color=theme.text if self.enabled else theme.muted_text,
            size=14,
            anchor="midleft",
        )
