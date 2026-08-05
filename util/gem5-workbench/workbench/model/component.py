"""Simulation component instances."""

from dataclasses import (
    dataclass,
    field,
)
from typing import Mapping

from workbench.model.identifiers import (
    ComponentId,
    new_component_id,
    require_identifier,
)
from workbench.model.values import (
    FrozenJsonValue,
    freeze_json_object,
)


@dataclass(frozen=True, slots=True)
class Component:
    type_id: str
    id: ComponentId = field(default_factory=new_component_id)
    parameters: Mapping[str, FrozenJsonValue] = field(default_factory=dict)

    def __post_init__(self) -> None:
        require_identifier(self.type_id, "component type id")
        require_identifier(self.id, "component id")
        if not isinstance(self.parameters, Mapping):
            raise ValueError("component parameters must be a mapping")
        parameters = freeze_json_object(
            dict(self.parameters), "component parameters"
        )
        object.__setattr__(self, "parameters", parameters)
