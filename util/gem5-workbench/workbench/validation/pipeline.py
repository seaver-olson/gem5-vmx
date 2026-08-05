"""Ordered validation pipeline for an already-decoded project document."""

from workbench.document import ProjectDocument
from workbench.registry import ComponentRegistry
from workbench.validation.diagnostics import Diagnostic
from workbench.validation.execution import validate_execution_readiness
from workbench.validation.readiness import validate_readiness
from workbench.validation.registry import validate_registry
from workbench.validation.structure import validate_structure


def validate_project(
    document: ProjectDocument,
    registry: ComponentRegistry,
) -> list[Diagnostic]:
    """Run every layer so unrelated components retain useful diagnostics."""

    diagnostics = validate_structure(document)
    diagnostics.extend(validate_registry(document, registry))
    diagnostics.extend(validate_readiness(document, registry))
    diagnostics.extend(validate_execution_readiness(document, registry))
    return diagnostics
