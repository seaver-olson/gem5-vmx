"""Definitions supplied by built-ins or future plugins."""

from dataclasses import dataclass, field
from enum import Enum

from workbench.model.values import JsonValue


class PortDirection(Enum):
    INPUT = "input"
    OUTPUT = "output"
    BIDIRECTIONAL = "bidirectional"


class ParameterType(Enum):
    BOOLEAN = "boolean"
    INTEGER = "integer"
    NUMBER = "number"
    STRING = "string"


@dataclass(frozen=True, slots=True)
class PortDefinition:
    id: str
    direction: PortDirection
    interface: str
    required: bool = False
    maximum_connections: int | None = 1
    vector: bool = False


@dataclass(frozen=True, slots=True)
class ParameterDefinition:
    id: str
    value_type: ParameterType
    default: JsonValue = None
    required: bool = False


@dataclass(frozen=True, slots=True)
class ComponentDefinition:
    type_id: str
    display_name: str
    category: str
    description: str = ""
    ports: dict[str, PortDefinition] = field(default_factory=dict)
    parameters: dict[str, ParameterDefinition] = field(default_factory=dict)
