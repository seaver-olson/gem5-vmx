"""Project aggregate and its invariant-preserving mutation API."""

from dataclasses import dataclass, field

from workbench.model.component import Component
from workbench.model.connection import Connection
from workbench.model.identifiers import ComponentId, ConnectionId
from workbench.model.values import JsonValue, clone_json_object


class ProjectMutationError(ValueError):
    """Raised when a requested mutation would violate project invariants."""


def _require_identifier(value: str, field: str) -> None:
    if not isinstance(value, str) or not value or value != value.strip():
        raise ProjectMutationError(
            f"{field} must be a non-empty string without surrounding whitespace"
        )


@dataclass(slots=True)
class Project:
    components: dict[ComponentId, Component] = field(default_factory=dict)
    connections: dict[ConnectionId, Connection] = field(default_factory=dict)
    metadata: dict[str, JsonValue] = field(default_factory=dict)

    def add_component(self, component: Component) -> None:
        _require_identifier(component.id, "component id")
        _require_identifier(component.type_id, "component type id")
        if component.id in self.components:
            raise ProjectMutationError(f"duplicate component id: {component.id}")
        try:
            component.parameters = clone_json_object(
                component.parameters, "component parameters"
            )
        except ValueError as error:
            raise ProjectMutationError(str(error)) from error
        self.components[component.id] = component

    def remove_component(self, component_id: ComponentId) -> Component:
        try:
            component = self.components.pop(component_id)
        except KeyError as error:
            raise ProjectMutationError(
                f"unknown component id: {component_id}"
            ) from error
        connected_ids = [
            connection_id
            for connection_id, connection in self.connections.items()
            if component_id
            in (connection.source.component_id, connection.target.component_id)
        ]
        for connection_id in connected_ids:
            del self.connections[connection_id]
        return component

    def add_connection(self, connection: Connection) -> None:
        _require_identifier(connection.id, "connection id")
        if connection.id in self.connections:
            raise ProjectMutationError(f"duplicate connection id: {connection.id}")
        for endpoint in (connection.source, connection.target):
            _require_identifier(endpoint.component_id, "endpoint component id")
            _require_identifier(endpoint.port_id, "endpoint port id")
            if endpoint.slot is not None and (
                isinstance(endpoint.slot, bool)
                or not isinstance(endpoint.slot, int)
                or endpoint.slot < 0
            ):
                raise ProjectMutationError(
                    "endpoint slot must be a non-negative integer or null"
                )
            if endpoint.component_id not in self.components:
                raise ProjectMutationError(
                    f"unknown endpoint component: {endpoint.component_id}"
                )
        try:
            connection.parameters = clone_json_object(
                connection.parameters, "connection parameters"
            )
        except ValueError as error:
            raise ProjectMutationError(str(error)) from error
        self.connections[connection.id] = connection

    def remove_connection(self, connection_id: ConnectionId) -> Connection:
        try:
            return self.connections.pop(connection_id)
        except KeyError as error:
            raise ProjectMutationError(
                f"unknown connection id: {connection_id}"
            ) from error

    def update_parameters(
        self,
        component_id: ComponentId,
        parameters: dict[str, JsonValue],
    ) -> None:
        try:
            component = self.components[component_id]
        except KeyError as error:
            raise ProjectMutationError(
                f"unknown component id: {component_id}"
            ) from error
        try:
            component.parameters = clone_json_object(
                parameters, "component parameters"
            )
        except ValueError as error:
            raise ProjectMutationError(str(error)) from error
