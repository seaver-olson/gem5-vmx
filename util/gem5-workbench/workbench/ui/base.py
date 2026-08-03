
from abc import ABC, abstractmethod

import pygame

from workbench.state import WorkbenchState
from workbench.ui.theme import Theme


class Panel(ABC):
    def __init__(self, rect: pygame.Rect, theme: Theme) -> None:
        self.rect = rect
        self.theme = theme

    def set_rect(self, rect: pygame.Rect) -> None:
        self.rect = rect

    @abstractmethod
    def handle_event(
        self,
        event: pygame.event.Event,
        state: WorkbenchState,
    ) -> bool:
        """Handle an event.

        Return True when the event was consumed and should not be sent
        to panels underneath this one.
        """

    def update(self, dt: float, state: WorkbenchState) -> None:
        """Update transient panel state.

        Panels without animations intentionally use this no-op implementation.
        """

    @abstractmethod
    def draw(
        self,
        surface: pygame.Surface,
        state: WorkbenchState,
    ) -> None:
        """Draw the panel."""
