"""Registry-independent validation of project relationships."""

from workbench.document import ProjectDocument
from workbench.validation.diagnostics import (
    Diagnostic,
    DiagnosticLayer,
    DiagnosticSeverity,
)


def validate_structure(document: ProjectDocument) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    project = document.project
    for component_id, component in project.components.items():
        if component_id != component.id:
            diagnostics.append(
                Diagnostic(
                    "structure.component_key",
                    "Component dictionary key does not match its ID",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.STRUCTURE,
                    component_id=component.id,
                )
            )
        if component_id not in document.layout.components:
            diagnostics.append(
                Diagnostic(
                    "structure.missing_layout",
                    "Component has no layout entry",
                    DiagnosticSeverity.WARNING,
                    DiagnosticLayer.STRUCTURE,
                    component_id=component_id,
                )
            )
    for layout_id in document.layout.components:
        if layout_id not in project.components:
            diagnostics.append(
                Diagnostic(
                    "structure.orphan_layout",
                    "Layout entry references an unknown component",
                    DiagnosticSeverity.WARNING,
                    DiagnosticLayer.STRUCTURE,
                    component_id=layout_id,
                )
            )
    for connection_id, connection in project.connections.items():
        if connection_id != connection.id:
            diagnostics.append(
                Diagnostic(
                    "structure.connection_key",
                    "Connection dictionary key does not match its ID",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.STRUCTURE,
                    connection_id=connection.id,
                )
            )
        for field, endpoint in (
            ("source", connection.source),
            ("target", connection.target),
        ):
            if endpoint.component_id not in project.components:
                diagnostics.append(
                    Diagnostic(
                        "structure.dangling_endpoint",
                        f"Connection {field} references an unknown component",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.STRUCTURE,
                        component_id=endpoint.component_id,
                        connection_id=connection.id,
                        field=field,
                    )
                )
    return diagnostics
