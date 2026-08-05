"""Opaque identifiers used by project entities."""

from typing import NewType
from uuid import uuid4

ComponentId = NewType("ComponentId", str)
ConnectionId = NewType("ConnectionId", str)


def require_identifier(value: object, field: str) -> None:
    """Reject identifiers which cannot be represented stably in JSON."""

    if not isinstance(value, str) or not value or value != value.strip():
        raise ValueError(
            f"{field} must be a non-empty string without surrounding whitespace"
        )


def new_component_id() -> ComponentId:
    return ComponentId(uuid4().hex)


def new_connection_id() -> ConnectionId:
    return ConnectionId(uuid4().hex)
