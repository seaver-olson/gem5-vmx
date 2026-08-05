"""Explicit connections between component ports."""

from dataclasses import (
    dataclass,
    field,
)
from typing import Mapping

from workbench.model.identifiers import (
    ComponentId,
    ConnectionId,
    new_connection_id,
    require_identifier,
)
from workbench.model.values import (
    FrozenJsonValue,
    freeze_json_object,
)


@dataclass(frozen=True, slots=True)
class Endpoint:
    component_id: ComponentId
    port_id: str
    slot: int | None = None

    def __post_init__(self) -> None:
        require_identifier(self.component_id, "endpoint component id")
        require_identifier(self.port_id, "endpoint port id")
        if self.slot is not None and (
            isinstance(self.slot, bool)
            or not isinstance(self.slot, int)
            or self.slot < 0
        ):
            raise ValueError(
                "endpoint slot must be a non-negative integer or null"
            )


@dataclass(frozen=True, slots=True)
class Connection:
    source: Endpoint
    target: Endpoint
    id: ConnectionId = field(default_factory=new_connection_id)
    parameters: Mapping[str, FrozenJsonValue] = field(default_factory=dict)

    def __post_init__(self) -> None:
        require_identifier(self.id, "connection id")
        if not isinstance(self.source, Endpoint) or not isinstance(
            self.target, Endpoint
        ):
            raise ValueError("connection endpoints must be Endpoint objects")
        if not isinstance(self.parameters, Mapping):
            raise ValueError("connection parameters must be a mapping")
        parameters = freeze_json_object(
            dict(self.parameters), "connection parameters"
        )
        object.__setattr__(self, "parameters", parameters)
