"""Registry-backed, category-grouped component palette."""

from dataclasses import dataclass

import pygame
from workbench.registry import (
    ComponentDefinition,
    ComponentRegistry,
    ComponentSupport,
    create_builtin_registry,
)
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import (
    Button,
    TextInput,
    display_text,
    draw_panel,
    draw_text,
    ellipsize,
    get_font,
)


@dataclass(frozen=True, slots=True)
class _PaletteRow:
    """One category header or component in the palette's scroll model."""

    category: str
    height: int
    definition: ComponentDefinition | None = None
    count: int = 0

    @property
    def is_category(self) -> bool:
        return self.definition is None


class Palette(Panel):
    """Browse runnable components or the full discovered catalog by category."""

    HEADER_HEIGHT = 84
    CATEGORY_HEIGHT = 28
    ITEM_HEIGHT = 38
    PADDING = 12
    ITEM_INDENT = 8
    FILTER_WIDTH = 82
    CONTROL_GAP = 8

    def __init__(
        self,
        rect: pygame.Rect,
        theme: Theme,
        registry: ComponentRegistry | None = None,
    ) -> None:
        super().__init__(rect, theme)
        self.registry = registry or create_builtin_registry()
        self._scroll = 0
        self._show_all = False
        self._collapsed_runnable: set[str] = set()
        self._collapsed_all = self._category_names()
        self._known_categories = self._category_names()
        self._search = TextInput(pygame.Rect(0, 0, 0, 28))
        self._filter_button = Button(
            "Runnable",
            pygame.Rect(0, 0, 0, 28),
            self._toggle_filter,
        )
        self._position_controls()

    @property
    def has_focus(self) -> bool:
        return self._search.focused

    @property
    def show_all(self) -> bool:
        """Whether unavailable and experimental definitions are visible."""

        return self._show_all

    def cancel_interactions(self) -> None:
        """Cancel pointer presses when the application loses focus."""

        self._filter_button.cancel_press()

    def set_registry(self, registry: ComponentRegistry) -> None:
        self.registry = registry
        categories = self._category_names()
        self._collapsed_runnable.intersection_update(categories)
        self._collapsed_all.intersection_update(categories)
        self._collapsed_all.update(categories - self._known_categories)
        self._known_categories = categories
        self._clamp_scroll()

    def _category_names(self) -> set[str]:
        return {
            definition.category.casefold()
            for definition in self.registry.definitions()
            if definition.support is not ComponentSupport.DEPRECATED
        }

    @property
    def _collapsed_categories(self) -> set[str]:
        return (
            self._collapsed_all if self._show_all else self._collapsed_runnable
        )

    def _toggle_filter(self) -> None:
        self._show_all = not self._show_all
        self._filter_button.label = "All" if self._show_all else "Runnable"
        self._scroll = 0

    @property
    def definitions(self) -> tuple[ComponentDefinition, ...]:
        """Definitions matching the support filter and current search text."""

        query = self._search.text.casefold().strip()
        definitions = []
        for definition in self.registry.definitions():
            if definition.support is ComponentSupport.DEPRECATED:
                continue
            if (
                not self._show_all
                and definition.support is not ComponentSupport.SUPPORTED
            ):
                continue
            haystack = " ".join(
                filter(
                    None,
                    (
                        definition.display_name,
                        definition.category,
                        definition.description,
                        definition.type_id,
                        definition.source_reference,
                        definition.support.value,
                        definition.support_reason,
                    ),
                )
            ).casefold()
            if query and query not in haystack:
                continue
            definitions.append(definition)
        return tuple(
            sorted(
                definitions,
                key=lambda item: (
                    item.category.casefold(),
                    item.support is not ComponentSupport.SUPPORTED,
                    item.display_name.casefold(),
                    item.type_id,
                ),
            )
        )

    def _rows(self) -> tuple[_PaletteRow, ...]:
        grouped: dict[str, list[ComponentDefinition]] = {}
        labels: dict[str, str] = {}
        for definition in self.definitions:
            category_key = definition.category.casefold()
            grouped.setdefault(category_key, []).append(definition)
            labels.setdefault(category_key, definition.category)

        searching = bool(self._search.text.strip())
        rows: list[_PaletteRow] = []
        for category_key in sorted(grouped):
            category = labels[category_key]
            definitions = grouped[category_key]
            rows.append(
                _PaletteRow(
                    category,
                    self.CATEGORY_HEIGHT,
                    count=len(definitions),
                )
            )
            if searching or category_key not in self._collapsed_categories:
                rows.extend(
                    _PaletteRow(
                        category,
                        self.ITEM_HEIGHT,
                        definition=definition,
                    )
                    for definition in definitions
                )
        return tuple(rows)

    def _position_controls(self) -> None:
        horizontal_padding = min(self.PADDING, max(0, self.rect.width) // 2)
        available = max(0, self.rect.width - horizontal_padding * 2)
        filter_width = min(self.FILTER_WIDTH, available)
        search_width = max(
            0,
            available
            - filter_width
            - (self.CONTROL_GAP if available > filter_width else 0),
        )
        control_y = self.rect.y + min(42, max(0, self.rect.height))
        control_height = max(0, min(28, self.rect.bottom - control_y))
        self._search.rect = pygame.Rect(
            self.rect.x + horizontal_padding,
            control_y,
            search_width,
            control_height,
        )
        self._filter_button.rect = pygame.Rect(
            self.rect.right - horizontal_padding - filter_width,
            control_y,
            filter_width,
            control_height,
        )

    def set_rect(self, rect: pygame.Rect) -> None:
        super().set_rect(rect)
        self._position_controls()
        self._clamp_scroll()

    def _content_rect(self) -> pygame.Rect:
        return pygame.Rect(
            self.rect.x,
            self.rect.y + self.HEADER_HEIGHT,
            self.rect.width,
            max(0, self.rect.height - self.HEADER_HEIGHT),
        )

    def _row_layouts(
        self,
        rows: tuple[_PaletteRow, ...] | None = None,
        *,
        visible_only: bool = False,
    ) -> tuple[tuple[_PaletteRow, pygame.Rect], ...]:
        rows = self._rows() if rows is None else rows
        content_rect = self._content_rect()
        y = self.rect.y + self.HEADER_HEIGHT - self._scroll
        layouts = []
        for row in rows:
            if visible_only and y >= content_rect.bottom:
                break
            row_visible = y + row.height > content_rect.top
            if row.is_category:
                rect = pygame.Rect(
                    self.rect.x + self.PADDING,
                    y,
                    max(0, self.rect.width - self.PADDING * 2),
                    row.height,
                )
            else:
                rect = pygame.Rect(
                    self.rect.x + self.PADDING + self.ITEM_INDENT,
                    y + 2,
                    max(
                        0,
                        self.rect.width - self.PADDING * 2 - self.ITEM_INDENT,
                    ),
                    max(0, row.height - 4),
                )
            if not visible_only or row_visible:
                layouts.append((row, rect))
            y += row.height
        return tuple(layouts)

    def _max_scroll(self, rows: tuple[_PaletteRow, ...] | None = None) -> int:
        rows = self._rows() if rows is None else rows
        content_height = sum(row.height for row in rows)
        return max(0, content_height - self._content_rect().height)

    def _clamp_scroll(
        self, rows: tuple[_PaletteRow, ...] | None = None
    ) -> None:
        self._scroll = max(0, min(self._scroll, self._max_scroll(rows)))

    def _toggle_category(self, category: str) -> None:
        if self._search.text.strip():
            return
        category = category.casefold()
        collapsed = self._collapsed_categories
        if category in collapsed:
            collapsed.remove(category)
        else:
            collapsed.add(category)
        self._clamp_scroll()

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        old_query = self._search.text
        if self._search.handle_event(event):
            if self._search.text != old_query:
                self._scroll = 0
            return True
        if self._filter_button.handle_event(event):
            return True
        if event.type == pygame.MOUSEWHEEL and self.rect.collidepoint(
            pygame.mouse.get_pos()
        ):
            self._scroll = max(
                0, min(self._max_scroll(), self._scroll - event.y * 30)
            )
            return True
        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if not self.rect.collidepoint(event.pos):
                return False
            content_rect = self._content_rect()
            if not content_rect.collidepoint(event.pos):
                return True
            for row, row_rect in self._row_layouts(visible_only=True):
                if not row_rect.collidepoint(event.pos):
                    continue
                if row.is_category:
                    self._toggle_category(row.category)
                    return True
                definition = row.definition
                if definition is None:
                    return True
                state.selected_type_id = definition.type_id
                state.selected_component_id = None
                state.selected_connection_id = None
                if definition.support is ComponentSupport.SUPPORTED:
                    state.status_message = (
                        f"{display_text(definition.display_name)} selected — "
                        "click the canvas to place it"
                    )
                else:
                    state.status_message = (
                        display_text(definition.support_reason)
                        or f"{display_text(definition.display_name)} "
                        f"is {definition.support.value}"
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
            self._draw_header(surface)
            content_rect = self._content_rect()
            rows = self._rows()
            self._clamp_scroll(rows)
            surface.set_clip(content_rect.clip(self.rect))
            self._draw_rows(
                surface,
                state,
                content_rect,
                rows,
                self._row_layouts(rows, visible_only=True),
            )
            self._draw_scrollbar(surface, content_rect, rows)
        finally:
            surface.set_clip(previous_clip)

    def _draw_header(self, surface: pygame.Surface) -> None:
        draw_text(
            surface,
            "Components",
            (self.rect.x + self.PADDING, self.rect.y + 14),
            color=self.theme.text,
            size=18,
            bold=True,
        )
        self._search.draw(surface, self.theme)
        if not self._search.text and not self._search.focused:
            draw_text(
                surface,
                "Search",
                (self._search.rect.x + 7, self._search.rect.centery),
                color=self.theme.muted_text,
                size=13,
                anchor="midleft",
            )
        self._filter_button.draw(surface, self.theme, pygame.mouse.get_pos())

    def _draw_rows(
        self,
        surface: pygame.Surface,
        state: WorkbenchState,
        content_rect: pygame.Rect,
        rows: tuple[_PaletteRow, ...],
        layouts: tuple[tuple[_PaletteRow, pygame.Rect], ...],
    ) -> None:
        if not rows:
            message = (
                "No matching components"
                if self._search.text.strip()
                else (
                    "No components"
                    if self._show_all
                    else "No runnable components"
                )
            )
            draw_text(
                surface,
                message,
                (content_rect.centerx, content_rect.y + 24),
                color=self.theme.muted_text,
                size=12,
                anchor="midtop",
            )
            return

        mouse_position = pygame.mouse.get_pos()
        label_font = get_font(13, bold=True)
        status_font = get_font(10)
        searching = bool(self._search.text.strip())
        for row, row_rect in layouts:
            if not row_rect.colliderect(content_rect):
                continue
            if row.is_category:
                self._draw_category_row(
                    surface,
                    row,
                    row_rect,
                    mouse_position,
                    searching,
                )
                continue
            definition = row.definition
            if definition is None:
                continue
            selected = state.selected_type_id == definition.type_id
            hovered = row_rect.collidepoint(mouse_position)
            fill = self.theme.button_hover if hovered else self.theme.button
            if selected:
                fill = self.theme.node_background
            pygame.draw.rect(surface, fill, row_rect, border_radius=5)
            if selected:
                pygame.draw.rect(
                    surface,
                    self.theme.accent,
                    row_rect,
                    width=2,
                    border_radius=5,
                )

            support_width = 0
            if definition.support is not ComponentSupport.SUPPORTED:
                support_label = display_text(definition.support.value.title())
                support_width = status_font.size(support_label)[0] + 10
                draw_text(
                    surface,
                    support_label,
                    (row_rect.right - 7, row_rect.centery),
                    color=self.theme.muted_text,
                    size=10,
                    anchor="midright",
                )
            max_width = max(0, row_rect.width - 18 - support_width)
            draw_text(
                surface,
                ellipsize(
                    display_text(definition.display_name),
                    label_font,
                    max_width,
                ),
                (row_rect.x + 9, row_rect.centery),
                color=self.theme.text,
                size=13,
                bold=True,
                anchor="midleft",
            )

    def _draw_category_row(
        self,
        surface: pygame.Surface,
        row: _PaletteRow,
        rect: pygame.Rect,
        mouse_position: tuple[int, int],
        searching: bool,
    ) -> None:
        if rect.collidepoint(mouse_position):
            pygame.draw.rect(surface, self.theme.button, rect, border_radius=4)
        expanded = (
            searching
            or row.category.casefold() not in self._collapsed_categories
        )
        if expanded:
            chevron_points = (
                (rect.x + 2, rect.centery - 3),
                (rect.x + 10, rect.centery - 3),
                (rect.x + 6, rect.centery + 2),
            )
        else:
            chevron_points = (
                (rect.x + 3, rect.centery - 5),
                (rect.x + 3, rect.centery + 5),
                (rect.x + 8, rect.centery),
            )
        pygame.draw.polygon(
            surface,
            self.theme.muted_text,
            chevron_points,
        )
        count_label = display_text(row.count)
        count_width = get_font(11).size(count_label)[0]
        draw_text(
            surface,
            count_label,
            (rect.right - 3, rect.centery),
            color=self.theme.muted_text,
            size=11,
            anchor="midright",
        )
        category_font = get_font(12, bold=True)
        draw_text(
            surface,
            ellipsize(
                display_text(row.category),
                category_font,
                max(0, rect.width - count_width - 25),
            ),
            (rect.x + 16, rect.centery),
            color=self.theme.text,
            size=12,
            bold=True,
            anchor="midleft",
        )

    def _draw_scrollbar(
        self,
        surface: pygame.Surface,
        content_rect: pygame.Rect,
        rows: tuple[_PaletteRow, ...] | None = None,
    ) -> None:
        max_scroll = self._max_scroll(rows)
        if max_scroll <= 0 or content_rect.height <= 0:
            return
        content_height = content_rect.height + max_scroll
        thumb_height = min(
            content_rect.height,
            max(
                18,
                round(
                    content_rect.height * content_rect.height / content_height
                ),
            ),
        )
        travel = max(0, content_rect.height - thumb_height)
        thumb_y = content_rect.y + round(travel * self._scroll / max_scroll)
        pygame.draw.rect(
            surface,
            self.theme.panel_border,
            pygame.Rect(content_rect.right - 4, thumb_y, 3, thumb_height),
            border_radius=2,
        )
