"""Definitions supplied by built-ins or future plugins."""

from dataclasses import (
    dataclass,
    field,
)
from enum import Enum
from types import MappingProxyType
from typing import Mapping

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
    ENUM = "enum"
    PATH = "path"


class ComponentSupport(Enum):
    SUPPORTED = "supported"
    EXPERIMENTAL = "experimental"
    UNAVAILABLE = "unavailable"
    DEPRECATED = "deprecated"


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
    has_default: bool = False
    required: bool = False
    choices: tuple[JsonValue, ...] = ()
    nullable: bool = False
    minimum: float | None = None
    maximum: float | None = None
    editor_hint: str | None = None
    read_only: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "choices", tuple(self.choices))


@dataclass(frozen=True, slots=True)
class ComponentDefinition:
    type_id: str
    display_name: str
    category: str
    description: str = ""
    ports: Mapping[str, PortDefinition] = field(default_factory=dict)
    parameters: Mapping[str, ParameterDefinition] = field(default_factory=dict)
    support: ComponentSupport = ComponentSupport.SUPPORTED
    support_reason: str = ""
    source_reference: str | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "ports", MappingProxyType(dict(self.ports)))
        object.__setattr__(
            self, "parameters", MappingProxyType(dict(self.parameters))
        )
