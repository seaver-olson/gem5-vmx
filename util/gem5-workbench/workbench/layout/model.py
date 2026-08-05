"""Validated JSON-compatible presentation data for the editor."""

import math
from dataclasses import (
    dataclass,
    field,
)
from types import MappingProxyType
from typing import (
    Iterable,
    Mapping,
)

from workbench.model.identifiers import ComponentId
from workbench.model.values import (
    FrozenJsonValue,
    JsonValue,
    freeze_json_object,
)

MAX_LAYOUT_COORDINATE = 1_000_000.0
MIN_VIEWPORT_ZOOM = 0.0001
MAX_VIEWPORT_ZOOM = 100.0


def _finite_number(value: float, field: str) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a number")
    try:
        finite = math.isfinite(value)
    except OverflowError:
        finite = False
    if not finite:
        raise ValueError(f"{field} must be finite")


def _component_id(value: object) -> None:
    if not isinstance(value, str) or not value or value != value.strip():
        raise ValueError(
            "layout component id must be a non-empty string without "
            "surrounding whitespace"
        )


@dataclass(frozen=True, slots=True)
class Position:
    x: float = 0.0
    y: float = 0.0

    def __post_init__(self) -> None:
        _finite_number(self.x, "position.x")
        _finite_number(self.y, "position.y")
        if abs(self.x) > MAX_LAYOUT_COORDINATE:
            raise ValueError(
                f"position.x must be within +/-{MAX_LAYOUT_COORDINATE:g}"
            )
        if abs(self.y) > MAX_LAYOUT_COORDINATE:
            raise ValueError(
                f"position.y must be within +/-{MAX_LAYOUT_COORDINATE:g}"
            )


@dataclass(frozen=True, slots=True)
class ComponentLayout:
    position: Position = field(default_factory=Position)
    collapsed: bool = False
    metadata: Mapping[str, FrozenJsonValue] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not isinstance(self.position, Position):
            raise ValueError("component layout position must be a Position")
        if not isinstance(self.collapsed, bool):
            raise ValueError("component layout collapsed must be boolean")
        if not isinstance(self.metadata, Mapping):
            raise ValueError("component layout metadata must be a mapping")
        metadata = freeze_json_object(dict(self.metadata), "layout metadata")
        object.__setattr__(self, "metadata", metadata)


@dataclass(frozen=True, slots=True)
class ViewportLayout:
    offset_x: float = 0.0
    offset_y: float = 0.0
    zoom: float = 1.0

    def __post_init__(self) -> None:
        _finite_number(self.offset_x, "viewport.offset_x")
        _finite_number(self.offset_y, "viewport.offset_y")
        _finite_number(self.zoom, "viewport.zoom")
        if abs(self.offset_x) > MAX_LAYOUT_COORDINATE:
            raise ValueError(
                f"viewport.offset_x must be within +/-{MAX_LAYOUT_COORDINATE:g}"
            )
        if abs(self.offset_y) > MAX_LAYOUT_COORDINATE:
            raise ValueError(
                f"viewport.offset_y must be within +/-{MAX_LAYOUT_COORDINATE:g}"
            )
        if not MIN_VIEWPORT_ZOOM <= self.zoom <= MAX_VIEWPORT_ZOOM:
            raise ValueError(
                "viewport.zoom must be between "
                f"{MIN_VIEWPORT_ZOOM:g} and {MAX_VIEWPORT_ZOOM:g}"
            )


class ProjectLayout:
    __slots__ = ("_components", "_viewport", "_metadata")

    def __init__(self) -> None:
        self._components: dict[ComponentId, ComponentLayout] = {}
        self._viewport = ViewportLayout()
        self._metadata: Mapping[str, FrozenJsonValue] = MappingProxyType({})

    @classmethod
    def _from_decoded(
        cls,
        components: Mapping[ComponentId, ComponentLayout],
        viewport: ViewportLayout,
        metadata: Mapping[str, JsonValue],
    ) -> "ProjectLayout":
        layout = cls()
        layout._components = dict(components)
        layout._viewport = viewport
        layout._metadata = freeze_json_object(metadata, "layout metadata")
        return layout

    @property
    def components(self) -> Mapping[ComponentId, ComponentLayout]:
        return MappingProxyType(self._components)

    @property
    def viewport(self) -> ViewportLayout:
        return self._viewport

    @property
    def metadata(self) -> Mapping[str, FrozenJsonValue]:
        return self._metadata

    def set_component(
        self,
        component_id: ComponentId,
        component_layout: ComponentLayout,
    ) -> None:
        _component_id(component_id)
        if not isinstance(component_layout, ComponentLayout):
            raise ValueError(
                "component layout must be a ComponentLayout object"
            )
        self._components[component_id] = component_layout

    def move_component(
        self, component_id: ComponentId, position: Position
    ) -> None:
        _component_id(component_id)
        if not isinstance(position, Position):
            raise ValueError("component position must be a Position")
        try:
            current = self._components[component_id]
        except KeyError as error:
            raise KeyError(
                f"missing component layout: {component_id}"
            ) from error
        self._components[component_id] = ComponentLayout(
            position, current.collapsed, current.metadata
        )

    def remove_component(self, component_id: ComponentId) -> None:
        _component_id(component_id)
        self._components.pop(component_id, None)

    def set_viewport(self, viewport: ViewportLayout) -> None:
        if not isinstance(viewport, ViewportLayout):
            raise ValueError("viewport must be a ViewportLayout object")
        self._viewport = viewport

    def update_metadata(self, metadata: Mapping[str, JsonValue]) -> None:
        if not isinstance(metadata, Mapping):
            raise ValueError("layout metadata must be a mapping")
        self._metadata = freeze_json_object(metadata, "layout metadata")

    def repair_missing(
        self,
        component_ids: Iterable[ComponentId],
        *,
        spacing: tuple[float, float] = (180.0, 100.0),
    ) -> list[ComponentId]:
        if not isinstance(spacing, tuple) or len(spacing) != 2:
            raise ValueError("layout spacing must contain two numbers")
        spacing_x, spacing_y = spacing
        _finite_number(spacing_x, "layout spacing.x")
        _finite_number(spacing_y, "layout spacing.y")

        # Reject an explicitly unusable spacing before preparing any entries.
        # This also preserves atomic failure for callers supplying custom
        # layout policies.
        Position(spacing_x, spacing_y)

        indexed_ids = tuple(enumerate(component_ids))
        for _, component_id in indexed_ids:
            _component_id(component_id)
        if not indexed_ids:
            return []

        def axis_capacity(step: float) -> int:
            if step == 0:
                return len(indexed_ids)
            return int(MAX_LAYOUT_COORDINATE // abs(step)) + 1

        maximum_columns = axis_capacity(spacing_x)
        maximum_rows = axis_capacity(spacing_y)
        # Retain the familiar four-column layout for ordinary projects, then
        # add columns only when that would push later rows out of bounds.
        column_count = min(
            maximum_columns,
            max(1, 4, math.ceil(len(indexed_ids) / maximum_rows)),
        )
        if math.ceil(len(indexed_ids) / column_count) > maximum_rows:
            raise ValueError(
                "component layout cannot fit within the supported coordinate range"
            )

        repaired: list[ComponentId] = []
        pending: dict[ComponentId, ComponentLayout] = {}
        known_ids = set(self._components)
        for index, component_id in indexed_ids:
            if component_id in known_ids:
                continue
            column = index % column_count
            row = index // column_count
            pending[component_id] = ComponentLayout(
                Position(column * spacing_x, row * spacing_y)
            )
            known_ids.add(component_id)
            repaired.append(component_id)
        self._components.update(pending)
        return repaired
