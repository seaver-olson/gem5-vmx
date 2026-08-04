"""Strict decoding and lossless persistence of project intent."""

import json
import math
from pathlib import Path
from typing import Any, NoReturn, cast

from workbench.document import (
    FORMAT_ID,
    SCHEMA_VERSION,
    ProjectDocument,
)
from workbench.layout import (
    ComponentLayout,
    Position,
    ProjectLayout,
    ViewportLayout,
)
from workbench.model import (
    Component,
    ComponentId,
    Connection,
    ConnectionId,
    Endpoint,
    Project,
)
from workbench.model.identifiers import new_component_id
from workbench.model.values import JsonValue
from workbench.validation import Diagnostic, validate_document_shape


class ProjectFileError(ValueError):
    def __init__(
        self,
        message: str,
        *,
        diagnostics: list[Diagnostic] | None = None,
    ) -> None:
        super().__init__(message)
        self.diagnostics = diagnostics or []


def _fail(message: str) -> NoReturn:
    raise ProjectFileError(message)


def _identifier(value: Any, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or value != value.strip()
    ):
        _fail(
            f"{field} must be a non-empty string without surrounding whitespace"
        )
    return value


def _number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail(f"{field} must be a number")
    try:
        finite = math.isfinite(value)
    except OverflowError:
        finite = False
    if not finite:
        _fail(f"{field} must be finite")
    return float(value)


def _json_value(value: Any, field: str) -> JsonValue:
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        _number(value, field)
        return value
    if isinstance(value, float):
        _number(value, field)
        return value
    if isinstance(value, list):
        return [
            _json_value(item, f"{field}[{index}]")
            for index, item in enumerate(value)
        ]
    if isinstance(value, dict):
        result: dict[str, JsonValue] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                _fail(f"{field} keys must be strings")
            result[key] = _json_value(item, f"{field}.{key}")
        return result
    _fail(f"{field} contains a non-JSON value")


def _object(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(f"{field} must be an object")
    return cast(dict[str, Any], value)


def _parameters(value: Any, field: str) -> dict[str, JsonValue]:
    result = _json_value(value, field)
    if not isinstance(result, dict):
        _fail(f"{field} must be an object")
    return result


def _endpoint(value: Any, field: str) -> Endpoint:
    data = _object(value, field)
    component_id = ComponentId(
        _identifier(data.get("component_id"), f"{field}.component_id")
    )
    port_id = _identifier(data.get("port_id"), f"{field}.port_id")
    slot = data.get("slot")
    if slot is not None and (
        isinstance(slot, bool) or not isinstance(slot, int) or slot < 0
    ):
        _fail(f"{field}.slot must be a non-negative integer or null")
    return Endpoint(component_id, port_id, slot)


def _legacy_document(data: dict[str, Any]) -> dict[str, Any] | None:
    if data.get("format") != FORMAT_ID or data.get("version") != 1:
        return None
    nodes = data.get("nodes")
    if not isinstance(nodes, list):
        return None
    components: list[dict[str, Any]] = []
    layouts: dict[str, Any] = {}
    for index, raw_node in enumerate(nodes):
        node = _object(raw_node, f"nodes[{index}]")
        node_id = node.get("id")
        if node_id is None:
            node_id = str(new_component_id())
        node_id = _identifier(node_id, f"nodes[{index}].id")
        position = node.get("position")
        if not isinstance(position, list) or len(position) != 2:
            _fail(f"nodes[{index}].position must contain two numbers")
        components.append(
            {
                "id": node_id,
                "type_id": node.get("kind"),
                "parameters": {},
            }
        )
        layouts[node_id] = {
            "position": {"x": position[0], "y": position[1]},
            "collapsed": False,
            "metadata": {},
        }
    return {
        "format": FORMAT_ID,
        "schema_version": SCHEMA_VERSION,
        "project": {
            "components": components,
            "connections": [],
            "metadata": {},
        },
        "layout": {
            "components": layouts,
            "viewport": {"offset_x": 0, "offset_y": 0, "zoom": 1},
            "metadata": {},
        },
    }


def document_from_data(value: Any) -> ProjectDocument:
    if not isinstance(value, dict):
        diagnostics = validate_document_shape(value)
        raise ProjectFileError("invalid project document", diagnostics=diagnostics)
    data = cast(dict[str, Any], value)
    legacy = _legacy_document(data)
    if legacy is not None:
        data = legacy
    diagnostics = validate_document_shape(data)
    if diagnostics:
        raise ProjectFileError("invalid project document", diagnostics=diagnostics)

    project_data = _object(data["project"], "project")
    components: dict[ComponentId, Component] = {}
    for index, raw_component in enumerate(project_data["components"]):
        field = f"project.components[{index}]"
        component_data = _object(raw_component, field)
        component_id = ComponentId(
            _identifier(component_data.get("id"), f"{field}.id")
        )
        if component_id in components:
            _fail(f"duplicate component id: {component_id}")
        type_id = _identifier(component_data.get("type_id"), f"{field}.type_id")
        parameters = _parameters(
            component_data.get("parameters", {}), f"{field}.parameters"
        )
        components[component_id] = Component(type_id, component_id, parameters)

    connections: dict[ConnectionId, Connection] = {}
    for index, raw_connection in enumerate(project_data["connections"]):
        field = f"project.connections[{index}]"
        connection_data = _object(raw_connection, field)
        connection_id = ConnectionId(
            _identifier(connection_data.get("id"), f"{field}.id")
        )
        if connection_id in connections:
            _fail(f"duplicate connection id: {connection_id}")
        source = _endpoint(connection_data.get("source"), f"{field}.source")
        target = _endpoint(connection_data.get("target"), f"{field}.target")
        parameters = _parameters(
            connection_data.get("parameters", {}), f"{field}.parameters"
        )
        connections[connection_id] = Connection(
            source, target, connection_id, parameters
        )

    project_metadata = _parameters(
        project_data.get("metadata", {}), "project.metadata"
    )
    project = Project(components, connections, project_metadata)

    layout_data = _object(data["layout"], "layout")
    layout_components: dict[ComponentId, ComponentLayout] = {}
    for raw_id, raw_layout in layout_data["components"].items():
        component_id = ComponentId(_identifier(raw_id, "layout component id"))
        component_data = _object(raw_layout, f"layout.components.{component_id}")
        position_data = _object(
            component_data.get("position"),
            f"layout.components.{component_id}.position",
        )
        position = Position(
            _number(
                position_data.get("x"),
                f"layout.components.{component_id}.position.x",
            ),
            _number(
                position_data.get("y"),
                f"layout.components.{component_id}.position.y",
            ),
        )
        collapsed = component_data.get("collapsed", False)
        if not isinstance(collapsed, bool):
            _fail(f"layout.components.{component_id}.collapsed must be boolean")
        metadata = _parameters(
            component_data.get("metadata", {}),
            f"layout.components.{component_id}.metadata",
        )
        layout_components[component_id] = ComponentLayout(
            position, collapsed, metadata
        )

    viewport_data = _object(layout_data.get("viewport", {}), "layout.viewport")
    viewport = ViewportLayout(
        _number(viewport_data.get("offset_x", 0), "layout.viewport.offset_x"),
        _number(viewport_data.get("offset_y", 0), "layout.viewport.offset_y"),
        _number(viewport_data.get("zoom", 1), "layout.viewport.zoom"),
    )
    if viewport.zoom <= 0:
        _fail("layout.viewport.zoom must be greater than zero")
    layout_metadata = _parameters(
        layout_data.get("metadata", {}), "layout.metadata"
    )
    layout = ProjectLayout(layout_components, viewport, layout_metadata)
    return ProjectDocument(project, layout)


def _endpoint_to_data(endpoint: Endpoint) -> dict[str, Any]:
    return {
        "component_id": str(endpoint.component_id),
        "port_id": endpoint.port_id,
        "slot": endpoint.slot,
    }


def document_to_data(document: ProjectDocument) -> dict[str, Any]:
    return {
        "format": FORMAT_ID,
        "schema_version": SCHEMA_VERSION,
        "project": {
            "components": [
                {
                    "id": str(component.id),
                    "type_id": component.type_id,
                    "parameters": component.parameters,
                }
                for component in document.project.components.values()
            ],
            "connections": [
                {
                    "id": str(connection.id),
                    "source": _endpoint_to_data(connection.source),
                    "target": _endpoint_to_data(connection.target),
                    "parameters": connection.parameters,
                }
                for connection in document.project.connections.values()
            ],
            "metadata": document.project.metadata,
        },
        "layout": {
            "components": {
                str(component_id): {
                    "position": {
                        "x": component_layout.position.x,
                        "y": component_layout.position.y,
                    },
                    "collapsed": component_layout.collapsed,
                    "metadata": component_layout.metadata,
                }
                for component_id, component_layout in document.layout.components.items()
            },
            "viewport": {
                "offset_x": document.layout.viewport.offset_x,
                "offset_y": document.layout.viewport.offset_y,
                "zoom": document.layout.viewport.zoom,
            },
            "metadata": document.layout.metadata,
        },
    }


def dumps_project_document(document: ProjectDocument) -> str:
    data = document_to_data(document)
    _json_value(data, "document")
    return json.dumps(data, indent=2, allow_nan=False) + "\n"


def loads_project_document(text: str) -> ProjectDocument:
    try:
        data = json.loads(
            text,
            parse_constant=lambda value: _fail(
                f"invalid JSON numeric constant: {value}"
            ),
        )
    except json.JSONDecodeError as error:
        raise ProjectFileError(f"invalid JSON: {error.msg}") from error
    return document_from_data(data)


def load_project_document(path: Path) -> ProjectDocument:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ProjectFileError(str(error)) from error
    return loads_project_document(text)


def save_project_document(path: Path, document: ProjectDocument) -> None:
    text = dumps_project_document(document)
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary_path.write_text(text, encoding="utf-8")
        temporary_path.replace(path)
    except OSError as error:
        raise ProjectFileError(str(error)) from error
