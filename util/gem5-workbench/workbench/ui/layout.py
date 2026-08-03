"""Responsive geometry for the workbench's top-level panels."""

from dataclasses import dataclass

import pygame

from workbench.constants import (
    INSPECTOR_WIDTH,
    PALETTE_WIDTH,
    STATUS_BAR_HEIGHT,
    TOOLBAR_HEIGHT,
)


@dataclass(frozen=True, slots=True)
class WorkbenchLayout:
    """Rectangles occupied by each region of the application window."""

    toolbar: pygame.Rect
    palette: pygame.Rect
    canvas: pygame.Rect
    inspector: pygame.Rect
    status_bar: pygame.Rect


def create_layout(window_size: tuple[int, int]) -> WorkbenchLayout:
    """Create a non-overlapping layout for ``window_size``.

    The application enforces a minimum window size, but keeping this function
    safe for tiny surfaces makes screenshots and unit tests less surprising.
    Side panels shrink proportionally when their preferred widths do not fit.
    """

    width = max(0, int(window_size[0]))
    height = max(0, int(window_size[1]))

    toolbar_height = min(TOOLBAR_HEIGHT, height)
    status_height = min(STATUS_BAR_HEIGHT, max(0, height - toolbar_height))
    content_y = toolbar_height
    content_height = max(0, height - toolbar_height - status_height)

    palette_width = min(PALETTE_WIDTH, width)
    inspector_width = min(INSPECTOR_WIDTH, max(0, width - palette_width))
    preferred_sides = PALETTE_WIDTH + INSPECTOR_WIDTH
    if width < preferred_sides and preferred_sides:
        palette_width = round(width * PALETTE_WIDTH / preferred_sides)
        inspector_width = width - palette_width

    canvas_width = max(0, width - palette_width - inspector_width)
    inspector_x = palette_width + canvas_width

    return WorkbenchLayout(
        toolbar=pygame.Rect(0, 0, width, toolbar_height),
        palette=pygame.Rect(0, content_y, palette_width, content_height),
        canvas=pygame.Rect(
            palette_width, content_y, canvas_width, content_height
        ),
        inspector=pygame.Rect(
            inspector_x, content_y, inspector_width, content_height
        ),
        status_bar=pygame.Rect(
            0, height - status_height, width, status_height
        ),
    )

