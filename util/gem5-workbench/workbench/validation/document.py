"""Validation of the serialized document envelope."""

from typing import Any

from workbench.document import FORMAT_ID, SCHEMA_VERSION
from workbench.validation.diagnostics import (
    Diagnostic,
    DiagnosticLayer,
    DiagnosticSeverity,
)


def _error(code: str, message: str, field: str) -> Diagnostic:
    return Diagnostic(
        code,
        message,
        DiagnosticSeverity.ERROR,
        DiagnosticLayer.DOCUMENT,
        field=field,
    )


def validate_document_shape(data: Any) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if not isinstance(data, dict):
        return [_error("document.not_object", "Document must be an object", "$")]
    if data.get("format") != FORMAT_ID:
        diagnostics.append(
            _error("document.format", "Unsupported document format", "format")
        )
    if data.get("schema_version") != SCHEMA_VERSION:
        diagnostics.append(
            _error(
                "document.schema_version",
                f"Supported schema version is {SCHEMA_VERSION}",
                "schema_version",
            )
        )
    project = data.get("project")
    layout = data.get("layout")
    if not isinstance(project, dict):
        diagnostics.append(
            _error("document.project", "Project section must be an object", "project")
        )
    else:
        if not isinstance(project.get("components"), list):
            diagnostics.append(
                _error(
                    "document.components",
                    "Project components must be a list",
                    "project.components",
                )
            )
        if not isinstance(project.get("connections"), list):
            diagnostics.append(
                _error(
                    "document.connections",
                    "Project connections must be a list",
                    "project.connections",
                )
            )
    if not isinstance(layout, dict):
        diagnostics.append(
            _error("document.layout", "Layout section must be an object", "layout")
        )
    elif not isinstance(layout.get("components"), dict):
        diagnostics.append(
            _error(
                "document.layout_components",
                "Layout components must be an object",
                "layout.components",
            )
        )
    return diagnostics
