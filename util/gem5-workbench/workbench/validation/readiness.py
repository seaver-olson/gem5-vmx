"""Execution-readiness checks which run after structural validation."""

from collections import Counter

from workbench.document import ProjectDocument
from workbench.registry import ComponentRegistry
from workbench.validation.diagnostics import (
    Diagnostic,
    DiagnosticLayer,
    DiagnosticSeverity,
)


def validate_readiness(
    document: ProjectDocument,
    registry: ComponentRegistry,
) -> list[Diagnostic]:
    usage: Counter[tuple[object, str]] = Counter()
    for connection in document.project.connections.values():
        usage[(connection.source.component_id, connection.source.port_id)] += 1
        usage[(connection.target.component_id, connection.target.port_id)] += 1
    diagnostics: list[Diagnostic] = []
    for component_id, component in document.project.components.items():
        definition = registry.get(component.type_id)
        if definition is None:
            continue
        for port_id, port in definition.ports.items():
            if port.required and usage[(component_id, port_id)] == 0:
                diagnostics.append(
                    Diagnostic(
                        "readiness.required_port",
                        f"Required port is not connected: {port_id}",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.READINESS,
                        component_id=component_id,
                        field=f"ports.{port_id}",
                    )
                )
    return diagnostics
