from dataclasses import dataclass, field
from uuid import uuid4

import pygame


@dataclass(slots=True)
class CanvasNode:
    kind: str
    position: pygame.Vector2
    id: str = field(default_factory=lambda: uuid4().hex)


@dataclass(slots=True)
class WorkbenchState:
    selected_component: str | None = None
    selected_node_id: str | None = None
    nodes: list[CanvasNode] = field(default_factory=list)
    status_message: str = "Ready"
