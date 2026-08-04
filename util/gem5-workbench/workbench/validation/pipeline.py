"""Ordered validation pipeline for an already-decoded project document."""

from workbench.document import ProjectDocument
from workbench.registry import ComponentRegistry
from workbench.validation.diagnostics import Diagnostic, DiagnosticSeverity
from workbench.validation.readiness import validate_readiness
from workbench.validation.registry import validate_registry
from workbench.validation.structure import validate_structure


def _has_errors(diagnostics: list[Diagnostic]) -> bool:
    return any(
        diagnostic.severity is DiagnosticSeverity.ERROR
        for diagnostic in diagnostics
    )


def validate_project(
    document: ProjectDocument,
    registry: ComponentRegistry,
) -> list[Diagnostic]:
    """Validate in layers, stopping dependent checks after an error layer."""

    diagnostics = validate_structure(document)
    if _has_errors(diagnostics):
        return diagnostics
    registry_diagnostics = validate_registry(document, registry)
    diagnostics.extend(registry_diagnostics)
    if _has_errors(registry_diagnostics):
        return diagnostics
    diagnostics.extend(validate_readiness(document, registry))
    return diagnostics
