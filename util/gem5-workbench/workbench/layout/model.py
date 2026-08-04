"""JSON-compatible presentation data stored alongside a project."""

from dataclasses import dataclass, field

from workbench.model.identifiers import ComponentId
from workbench.model.values import JsonValue


@dataclass(slots=True)
class Position:
    x: float = 0.0
    y: float = 0.0


@dataclass(slots=True)
class ComponentLayout:
    position: Position = field(default_factory=Position)
    collapsed: bool = False
    metadata: dict[str, JsonValue] = field(default_factory=dict)


@dataclass(slots=True)
class ViewportLayout:
    offset_x: float = 0.0
    offset_y: float = 0.0
    zoom: float = 1.0


@dataclass(slots=True)
class ProjectLayout:
    components: dict[ComponentId, ComponentLayout] = field(default_factory=dict)
    viewport: ViewportLayout = field(default_factory=ViewportLayout)
    metadata: dict[str, JsonValue] = field(default_factory=dict)

    def set_component(
        self,
        component_id: ComponentId,
        component_layout: ComponentLayout,
    ) -> None:
        self.components[component_id] = component_layout

    def remove_component(self, component_id: ComponentId) -> None:
        self.components.pop(component_id, None)
