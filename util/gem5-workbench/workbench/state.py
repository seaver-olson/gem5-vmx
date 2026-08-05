"""Transient workbench state; persistent data lives in ProjectDocument."""

from dataclasses import (
    dataclass,
    field,
)

from workbench.document import ProjectDocument
from workbench.model.identifiers import (
    ComponentId,
    ConnectionId,
)
from workbench.validation import Diagnostic


@dataclass(slots=True)
class WorkbenchState:
    document: ProjectDocument = field(default_factory=ProjectDocument)
    selected_type_id: str | None = None
    selected_component_id: ComponentId | None = None
    selected_connection_id: ConnectionId | None = None
    diagnostics: list[Diagnostic] = field(default_factory=list)
    status_message: str = "Ready"
    validated_revision: int = -1
    catalog_message: str = "Catalog not loaded"
    catalog_busy: bool = False
    gem5_binary: str | None = None
    job_kind: str | None = None
    job_state: str = "idle"
    job_log_lines: list[str] = field(default_factory=list)
    job_output_path: str | None = None
    can_build: bool = False
    can_run: bool = False
    run_block_reason: str | None = None
