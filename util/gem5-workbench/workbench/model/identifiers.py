"""Opaque identifiers used by project entities."""

from typing import NewType
from uuid import uuid4


ComponentId = NewType("ComponentId", str)
ConnectionId = NewType("ConnectionId", str)


def new_component_id() -> ComponentId:
    return ComponentId(uuid4().hex)


def new_connection_id() -> ConnectionId:
    return ConnectionId(uuid4().hex)
