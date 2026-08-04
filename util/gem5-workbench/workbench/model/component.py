"""Simulation component instances."""

from dataclasses import dataclass, field

from workbench.model.identifiers import ComponentId, new_component_id
from workbench.model.values import JsonValue


@dataclass(slots=True)
class Component:
    type_id: str
    id: ComponentId = field(default_factory=new_component_id)
    parameters: dict[str, JsonValue] = field(default_factory=dict)
