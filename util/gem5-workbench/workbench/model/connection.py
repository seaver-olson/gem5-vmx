"""Explicit connections between component ports."""

from dataclasses import dataclass, field

from workbench.model.identifiers import (
    ComponentId,
    ConnectionId,
    new_connection_id,
)
from workbench.model.values import JsonValue


@dataclass(frozen=True, slots=True)
class Endpoint:
    component_id: ComponentId
    port_id: str
    slot: int | None = None


@dataclass(slots=True)
class Connection:
    source: Endpoint
    target: Endpoint
    id: ConnectionId = field(default_factory=new_connection_id)
    parameters: dict[str, JsonValue] = field(default_factory=dict)
