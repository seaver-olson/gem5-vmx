"""Transient workbench state; persistent data lives in ProjectDocument."""

from dataclasses import dataclass, field

from workbench.document import ProjectDocument
from workbench.model.identifiers import ComponentId, ConnectionId
from workbench.validation import Diagnostic


@dataclass(slots=True)
class WorkbenchState:
    document: ProjectDocument = field(default_factory=ProjectDocument)
    selected_type_id: str | None = None
    selected_component_id: ComponentId | None = None
    selected_connection_id: ConnectionId | None = None
    diagnostics: list[Diagnostic] = field(default_factory=list)
    status_message: str = "Ready"
