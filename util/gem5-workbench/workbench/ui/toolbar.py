"""Top toolbar for project-level workbench actions."""

from collections.abc import Callable

import pygame

from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import Button, draw_panel, draw_text


class Toolbar(Panel):
    """Project actions which remain available across all workbench views."""

    def __init__(
        self,
        rect: pygame.Rect,
        theme: Theme,
        *,
        on_new: Callable[[], None] | None = None,
        on_save: Callable[[], None] | None = None,
        on_load: Callable[[], None] | None = None,
    ) -> None:
        super().__init__(rect, theme)
        self._actions = (
            ("New", on_new),
            ("Save", on_save),
            ("Load", on_load),
        )
        self._buttons: list[Button] = []
        self._rebuild_buttons()

    def set_rect(self, rect: pygame.Rect) -> None:
        super().set_rect(rect)
        self._rebuild_buttons()

    def _rebuild_buttons(self) -> None:
        self._buttons.clear()
        x = self.rect.x + 198
        height = min(34, max(0, self.rect.height - 16))
        y = self.rect.centery - height // 2
        for label, callback in self._actions:
            width = 70
            self._buttons.append(
                Button(label, pygame.Rect(x, y, width, height), callback)
            )
            x += width + 8

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        return any(button.handle_event(event) for button in self._buttons)

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        draw_panel(surface, self.rect, self.theme)
        if self.rect.width < 1 or self.rect.height < 1:
            return
        draw_text(
            surface,
            "gem5 Workbench",
            (self.rect.x + 16, self.rect.centery),
            color=self.theme.text,
            size=20,
            bold=True,
            anchor="midleft",
        )
        mouse_position = pygame.mouse.get_pos()
        for button in self._buttons:
            if button.rect.right <= self.rect.right - 8:
                button.draw(surface, self.theme, mouse_position)
