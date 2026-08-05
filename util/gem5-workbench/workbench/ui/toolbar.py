"""Top toolbar for project-level workbench actions."""

from collections.abc import Callable

import pygame
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import (
    Button,
    draw_panel,
    draw_text,
)


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
        on_build: Callable[[], None] | None = None,
        on_refresh: Callable[[], None] | None = None,
        on_run: Callable[[], None] | None = None,
        on_stop: Callable[[], None] | None = None,
    ) -> None:
        super().__init__(rect, theme)
        self._actions = (
            ("New", on_new),
            ("Save", on_save),
            ("Load", on_load),
            ("Build", on_build),
            ("Refresh", on_refresh),
            ("Run", on_run),
            ("Stop", on_stop),
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
            width = 76 if label == "Refresh" else 68
            self._buttons.append(
                Button(label, pygame.Rect(x, y, width, height), callback)
            )
            x += width + 8

    def _sync_enabled(self, state: WorkbenchState) -> None:
        active = state.job_state not in (
            "idle",
            "succeeded",
            "failed",
            "cancelled",
        )
        for button in self._buttons:
            if button.label == "Build":
                button.enabled = (
                    state.can_build and not active and not state.catalog_busy
                )
            elif button.label == "Refresh":
                button.enabled = not active and not state.catalog_busy
            elif button.label == "Run":
                button.enabled = (
                    state.can_run and not active and not state.catalog_busy
                )
            elif button.label == "Stop":
                button.enabled = active

    def cancel_interactions(self) -> None:
        """Cancel pointer presses when the application loses focus."""

        for button in self._buttons:
            button.cancel_press()

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        self._sync_enabled(state)
        return any(
            button.handle_event(event)
            for button in self._buttons
            if button.rect.right <= self.rect.right - 8
        )

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        draw_panel(surface, self.rect, self.theme)
        if self.rect.width < 1 or self.rect.height < 1:
            return
        self._sync_enabled(state)
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
