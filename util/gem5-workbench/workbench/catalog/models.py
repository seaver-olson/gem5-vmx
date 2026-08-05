"""Immutable models shared by catalog discovery, probing, and caching."""

from __future__ import annotations

import math
from dataclasses import (
    dataclass,
    field,
)
from enum import Enum
from types import MappingProxyType
from typing import Mapping


class SupportStatus(Enum):
    """How confidently Workbench can use a discovered symbol."""

    SOURCE_ONLY = "source_only"
    RUNTIME_CONFIRMED = "runtime_confirmed"
    UNAVAILABLE = "unavailable"


class SymbolKind(Enum):
    CLASS = "class"
    FACTORY = "factory"


class ParameterKind(Enum):
    BOOLEAN = "boolean"
    INTEGER = "integer"
    NUMBER = "number"
    STRING = "string"
    ENUM = "enum"
    UNKNOWN = "unknown"


class DiagnosticSeverity(Enum):
    INFO = "info"
    WARNING = "warning"
    ERROR = "error"


def _freeze(value: object) -> object:
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("catalog values must contain finite numbers")
        return value
    if isinstance(value, Mapping):
        if not all(isinstance(key, str) for key in value):
            raise TypeError("catalog mapping keys must be strings")
        return MappingProxyType(
            {key: _freeze(item) for key, item in value.items()}
        )
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(item) for item in value)
    raise TypeError("catalog values must be JSON-compatible")


def thaw_catalog_value(value: object) -> object:
    """Convert an immutable catalog value into JSON-compatible containers."""

    if isinstance(value, Mapping):
        return {
            str(key): thaw_catalog_value(item) for key, item in value.items()
        }
    if isinstance(value, tuple):
        return [thaw_catalog_value(item) for item in value]
    return value


@dataclass(frozen=True, slots=True)
class CatalogDiagnostic:
    code: str
    message: str
    severity: DiagnosticSeverity = DiagnosticSeverity.WARNING
    path: str | None = None
    line: int | None = None
    type_id: str | None = None


@dataclass(frozen=True, slots=True)
class SymbolReference:
    module: str
    qualified_name: str
    source_path: str
    line: int


@dataclass(frozen=True, slots=True)
class ParameterSpec:
    id: str
    kind: ParameterKind = ParameterKind.UNKNOWN
    annotation: str | None = None
    required: bool = True
    has_default: bool = False
    default: object = None
    default_expression: str | None = None
    default_is_literal: bool = False
    nullable: bool = False
    choices: tuple[str, ...] = ()
    positional_only: bool = False
    keyword_only: bool = False
    variadic: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "default", _freeze(self.default))
        object.__setattr__(self, "choices", tuple(self.choices))


@dataclass(frozen=True, slots=True)
class CatalogEntry:
    type_id: str
    display_name: str
    category: str
    symbol: SymbolReference
    symbol_kind: SymbolKind
    parameters: tuple[ParameterSpec, ...] = ()
    description: str = ""
    support_status: SupportStatus = SupportStatus.SOURCE_ONLY
    is_abstract: bool = False
    source_fingerprint: str = ""
    diagnostics: tuple[CatalogDiagnostic, ...] = ()
    metadata: Mapping[str, object] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "parameters", tuple(self.parameters))
        object.__setattr__(self, "diagnostics", tuple(self.diagnostics))
        object.__setattr__(self, "metadata", _freeze(self.metadata))

    def parameter(self, parameter_id: str) -> ParameterSpec | None:
        return next(
            (
                parameter
                for parameter in self.parameters
                if parameter.id == parameter_id
            ),
            None,
        )

    @property
    def can_place(self) -> bool:
        return (
            self.support_status is SupportStatus.RUNTIME_CONFIRMED
            and not self.is_abstract
        )


@dataclass(frozen=True, slots=True)
class Catalog:
    source_root: str
    entries: tuple[CatalogEntry, ...]
    source_fingerprint: str
    diagnostics: tuple[CatalogDiagnostic, ...] = ()
    binary_fingerprint: str | None = None
    supported_isas: tuple[str, ...] = ()
    supported_protocols: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "entries", tuple(self.entries))
        object.__setattr__(self, "diagnostics", tuple(self.diagnostics))
        object.__setattr__(self, "supported_isas", tuple(self.supported_isas))
        object.__setattr__(
            self, "supported_protocols", tuple(self.supported_protocols)
        )
        type_ids = [entry.type_id for entry in self.entries]
        if len(type_ids) != len(set(type_ids)):
            raise ValueError("catalog entries must have unique type IDs")

    def get(self, type_id: str) -> CatalogEntry | None:
        return next(
            (entry for entry in self.entries if entry.type_id == type_id),
            None,
        )


@dataclass(frozen=True, slots=True)
class RuntimeParameter:
    id: str
    annotation: str | None = None
    choices: tuple[str, ...] = ()
    has_default: bool = False
    default_expression: str | None = None
    positional_only: bool = False
    keyword_only: bool = False
    variadic: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "choices", tuple(self.choices))


@dataclass(frozen=True, slots=True)
class RuntimeSymbol:
    type_id: str
    available: bool
    symbol_kind: SymbolKind | None = None
    parameters: tuple[RuntimeParameter, ...] = ()
    error: str | None = None
    is_abstract: bool | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "parameters", tuple(self.parameters))


@dataclass(frozen=True, slots=True)
class RuntimeProbeResult:
    source_fingerprint: str
    binary_fingerprint: str
    symbols: tuple[RuntimeSymbol, ...]
    supported_isas: tuple[str, ...] = ()
    supported_protocols: tuple[str, ...] = ()
    diagnostics: tuple[CatalogDiagnostic, ...] = ()
    stdout: str = ""
    stderr: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "symbols", tuple(self.symbols))
        object.__setattr__(self, "supported_isas", tuple(self.supported_isas))
        object.__setattr__(
            self, "supported_protocols", tuple(self.supported_protocols)
        )
        object.__setattr__(self, "diagnostics", tuple(self.diagnostics))
        type_ids = [symbol.type_id for symbol in self.symbols]
        if len(type_ids) != len(set(type_ids)):
            raise ValueError("runtime symbols must have unique type IDs")

    def get(self, type_id: str) -> RuntimeSymbol | None:
        return next(
            (symbol for symbol in self.symbols if symbol.type_id == type_id),
            None,
        )


@dataclass(frozen=True, slots=True)
class CatalogInstallationIdentity:
    """Source/binary identity for catalog cache selection, not execution."""

    source_root: str
    binary_path: str | None = None
    source_fingerprint: str | None = None
    binary_fingerprint: str | None = None
