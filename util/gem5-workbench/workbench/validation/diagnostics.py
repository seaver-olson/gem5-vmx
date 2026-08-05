"""Structured validation results suitable for both UI and logs."""

from dataclasses import dataclass
from enum import Enum

from workbench.model.identifiers import (
    ComponentId,
    ConnectionId,
)


class DiagnosticSeverity(Enum):
    INFO = "info"
    WARNING = "warning"
    ERROR = "error"


class DiagnosticLayer(Enum):
    DOCUMENT = "document"
    STRUCTURE = "structure"
    REGISTRY = "registry"
    READINESS = "readiness"
    EXECUTION = "execution"


@dataclass(frozen=True, slots=True)
class Diagnostic:
    code: str
    message: str
    severity: DiagnosticSeverity
    layer: DiagnosticLayer
    component_id: ComponentId | None = None
    connection_id: ConnectionId | None = None
    field: str | None = None
