"""Project aggregate and its invariant-preserving mutation API."""

from types import MappingProxyType
from typing import Mapping

from workbench.model.component import Component
from workbench.model.connection import Connection
from workbench.model.identifiers import (
    ComponentId,
    ConnectionId,
    require_identifier,
)
from workbench.model.values import (
    FrozenJsonValue,
    JsonValue,
    freeze_json_object,
)


class ProjectMutationError(ValueError):
    """Raised when a mutation would violate a project invariant."""


def _require_identifier(value: str, field: str) -> None:
    try:
        require_identifier(value, field)
    except ValueError as error:
        raise ProjectMutationError(str(error)) from error


class Project:
    __slots__ = (
        "_components",
        "_connections",
        "_metadata",
        "_revision",
    )

    def __init__(self) -> None:
        self._components: dict[ComponentId, Component] = {}
        self._connections: dict[ConnectionId, Connection] = {}
        self._metadata: Mapping[str, FrozenJsonValue] = MappingProxyType({})
        self._revision = 0

    @classmethod
    def _from_decoded(
        cls,
        components: Mapping[ComponentId, Component],
        connections: Mapping[ConnectionId, Connection],
        metadata: Mapping[str, JsonValue],
    ) -> "Project":
        """Construct decoded state; cross-reference errors remain diagnostics."""

        project = cls()
        project._components = dict(components)
        project._connections = dict(connections)
        project._metadata = freeze_json_object(metadata, "project metadata")
        project._revision = 0
        return project

    @property
    def components(self) -> Mapping[ComponentId, Component]:
        return MappingProxyType(self._components)

    @property
    def connections(self) -> Mapping[ConnectionId, Connection]:
        return MappingProxyType(self._connections)

    @property
    def metadata(self) -> Mapping[str, FrozenJsonValue]:
        return self._metadata

    @property
    def revision(self) -> int:
        """Return the transient mutation serial used by UI adapters."""

        return self._revision

    def add_component(self, component: Component) -> None:
        if not isinstance(component, Component):
            raise ProjectMutationError("component must be a Component object")
        _require_identifier(component.id, "component id")
        _require_identifier(component.type_id, "component type id")
        if component.id in self._components:
            raise ProjectMutationError(
                f"duplicate component id: {component.id}"
            )
        self._components[component.id] = component
        self._revision += 1

    def remove_component(self, component_id: ComponentId) -> Component:
        _require_identifier(component_id, "component id")
        try:
            component = self._components.pop(component_id)
        except KeyError as error:
            raise ProjectMutationError(
                f"unknown component id: {component_id}"
            ) from error
        connected_ids = [
            connection_id
            for connection_id, connection in self._connections.items()
            if component_id
            in (connection.source.component_id, connection.target.component_id)
        ]
        for connection_id in connected_ids:
            del self._connections[connection_id]
        self._revision += 1
        return component

    def add_connection(self, connection: Connection) -> None:
        if not isinstance(connection, Connection):
            raise ProjectMutationError(
                "connection must be a Connection object"
            )
        _require_identifier(connection.id, "connection id")
        if connection.id in self._connections:
            raise ProjectMutationError(
                f"duplicate connection id: {connection.id}"
            )
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
            if endpoint.component_id not in self._components:
                raise ProjectMutationError(
                    f"unknown endpoint component: {endpoint.component_id}"
                )
        self._connections[connection.id] = connection
        self._revision += 1

    def remove_connection(self, connection_id: ConnectionId) -> Connection:
        _require_identifier(connection_id, "connection id")
        try:
            connection = self._connections.pop(connection_id)
        except KeyError as error:
            raise ProjectMutationError(
                f"unknown connection id: {connection_id}"
            ) from error
        self._revision += 1
        return connection

    def update_parameters(
        self,
        component_id: ComponentId,
        parameters: Mapping[str, JsonValue],
    ) -> Component:
        _require_identifier(component_id, "component id")
        try:
            component = self._components[component_id]
        except KeyError as error:
            raise ProjectMutationError(
                f"unknown component id: {component_id}"
            ) from error
        try:
            updated = Component(component.type_id, component.id, parameters)
        except ValueError as error:
            raise ProjectMutationError(str(error)) from error
        self._components[component_id] = updated
        self._revision += 1
        return updated

    def update_metadata(self, metadata: Mapping[str, JsonValue]) -> None:
        if not isinstance(metadata, Mapping):
            raise ProjectMutationError("project metadata must be a mapping")
        try:
            updated = freeze_json_object(metadata, "project metadata")
        except ValueError as error:
            raise ProjectMutationError(str(error)) from error
        self._metadata = updated
        self._revision += 1
