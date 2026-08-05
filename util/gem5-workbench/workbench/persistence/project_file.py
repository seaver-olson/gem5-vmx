"""Schema-validating decoding and persistence of project intent."""

import json
import math
import os
import tempfile
from pathlib import Path
from typing import (
    Any,
    NoReturn,
    cast,
)

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
from workbench.model.values import (
    MAX_JSON_NESTING,
    JsonValue,
    thaw_json_object,
)
from workbench.validation import (
    Diagnostic,
    validate_document_shape,
)


class ProjectFileError(ValueError):
    def __init__(
        self,
        message: str,
        *,
        diagnostics: list[Diagnostic] | None = None,
    ) -> None:
        super().__init__(message)
        self.diagnostics = diagnostics or []


MAX_PROJECT_FILE_BYTES = 64 * 1024 * 1024


def _fail(message: str) -> NoReturn:
    raise ProjectFileError(message)


def _identifier(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
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


def _json_value(value: Any, field: str, depth: int = 0) -> JsonValue:
    if depth > MAX_JSON_NESTING:
        _fail(
            f"{field} exceeds the maximum JSON nesting depth of "
            f"{MAX_JSON_NESTING}"
        )
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
            _json_value(item, f"{field}[{index}]", depth + 1)
            for index, item in enumerate(value)
        ]
    if isinstance(value, dict):
        result: dict[str, JsonValue] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                _fail(f"{field} keys must be strings")
            result[key] = _json_value(item, f"{field}.{key}", depth + 1)
        return result
    _fail(f"{field} contains a non-JSON value")


def _object(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(f"{field} must be an object")
    return cast(dict[str, Any], value)


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build a JSON object while rejecting ambiguous duplicate keys."""

    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            _fail(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _reject_unknown_keys(
    data: dict[str, Any], allowed: set[str], field: str
) -> None:
    unknown = sorted(repr(key) for key in data if key not in allowed)
    if unknown:
        _fail(f"{field} contains unknown field(s): {', '.join(unknown)}")


def _parameters(value: Any, field: str) -> dict[str, JsonValue]:
    result = _json_value(value, field)
    if not isinstance(result, dict):
        _fail(f"{field} must be an object")
    return result


def _endpoint(value: Any, field: str) -> Endpoint:
    data = _object(value, field)
    _reject_unknown_keys(data, {"component_id", "port_id", "slot"}, field)
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
    version = data.get("version")
    if (
        data.get("format") != FORMAT_ID
        or isinstance(version, bool)
        or not isinstance(version, int)
        or version != 1
    ):
        return None
    _reject_unknown_keys(data, {"format", "version", "nodes"}, "$ (legacy)")
    nodes = data.get("nodes")
    if not isinstance(nodes, list):
        return None
    components: list[dict[str, Any]] = []
    layouts: dict[str, Any] = {}
    for index, raw_node in enumerate(nodes):
        node = _object(raw_node, f"nodes[{index}]")
        _reject_unknown_keys(
            node, {"id", "kind", "position"}, f"nodes[{index}]"
        )
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
        raise ProjectFileError(
            "invalid project document", diagnostics=diagnostics
        )
    data = cast(dict[str, Any], value)
    legacy = _legacy_document(data)
    if legacy is not None:
        data = legacy
    _reject_unknown_keys(
        data, {"format", "schema_version", "project", "layout"}, "$"
    )
    diagnostics = validate_document_shape(data)
    if diagnostics:
        raise ProjectFileError(
            "invalid project document", diagnostics=diagnostics
        )

    project_data = _object(data["project"], "project")
    _reject_unknown_keys(
        project_data, {"components", "connections", "metadata"}, "project"
    )
    components: dict[ComponentId, Component] = {}
    for index, raw_component in enumerate(project_data["components"]):
        field = f"project.components[{index}]"
        component_data = _object(raw_component, field)
        _reject_unknown_keys(
            component_data, {"id", "type_id", "parameters"}, field
        )
        component_id = ComponentId(
            _identifier(component_data.get("id"), f"{field}.id")
        )
        if component_id in components:
            _fail(f"duplicate component id: {component_id}")
        type_id = _identifier(
            component_data.get("type_id"), f"{field}.type_id"
        )
        parameters = _parameters(
            component_data.get("parameters", {}), f"{field}.parameters"
        )
        components[component_id] = Component(type_id, component_id, parameters)

    connections: dict[ConnectionId, Connection] = {}
    for index, raw_connection in enumerate(project_data["connections"]):
        field = f"project.connections[{index}]"
        connection_data = _object(raw_connection, field)
        _reject_unknown_keys(
            connection_data,
            {"id", "source", "target", "parameters"},
            field,
        )
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
    project = Project._from_decoded(components, connections, project_metadata)

    layout_data = _object(data["layout"], "layout")
    _reject_unknown_keys(
        layout_data, {"components", "viewport", "metadata"}, "layout"
    )
    layout_components: dict[ComponentId, ComponentLayout] = {}
    for raw_id, raw_layout in layout_data["components"].items():
        component_id = ComponentId(_identifier(raw_id, "layout component id"))
        component_data = _object(
            raw_layout, f"layout.components.{component_id}"
        )
        _reject_unknown_keys(
            component_data,
            {"position", "collapsed", "metadata"},
            f"layout.components.{component_id}",
        )
        position_data = _object(
            component_data.get("position"),
            f"layout.components.{component_id}.position",
        )
        _reject_unknown_keys(
            position_data,
            {"x", "y"},
            f"layout.components.{component_id}.position",
        )
        try:
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
        except ValueError as error:
            _fail(str(error))
        collapsed = component_data.get("collapsed", False)
        if not isinstance(collapsed, bool):
            _fail(
                f"layout.components.{component_id}.collapsed must be boolean"
            )
        metadata = _parameters(
            component_data.get("metadata", {}),
            f"layout.components.{component_id}.metadata",
        )
        layout_components[component_id] = ComponentLayout(
            position, collapsed, metadata
        )

    viewport_data = _object(layout_data.get("viewport", {}), "layout.viewport")
    _reject_unknown_keys(
        viewport_data,
        {"offset_x", "offset_y", "zoom"},
        "layout.viewport",
    )
    try:
        viewport = ViewportLayout(
            _number(
                viewport_data.get("offset_x", 0), "layout.viewport.offset_x"
            ),
            _number(
                viewport_data.get("offset_y", 0), "layout.viewport.offset_y"
            ),
            _number(viewport_data.get("zoom", 1), "layout.viewport.zoom"),
        )
    except ValueError as error:
        _fail(str(error))
    layout_metadata = _parameters(
        layout_data.get("metadata", {}), "layout.metadata"
    )
    layout = ProjectLayout._from_decoded(
        layout_components, viewport, layout_metadata
    )
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
                    "parameters": thaw_json_object(component.parameters),
                }
                for component in document.project.components.values()
            ],
            "connections": [
                {
                    "id": str(connection.id),
                    "source": _endpoint_to_data(connection.source),
                    "target": _endpoint_to_data(connection.target),
                    "parameters": thaw_json_object(connection.parameters),
                }
                for connection in document.project.connections.values()
            ],
            "metadata": thaw_json_object(document.project.metadata),
        },
        "layout": {
            "components": {
                str(component_id): {
                    "position": {
                        "x": component_layout.position.x,
                        "y": component_layout.position.y,
                    },
                    "collapsed": component_layout.collapsed,
                    "metadata": thaw_json_object(component_layout.metadata),
                }
                for component_id, component_layout in document.layout.components.items()
            },
            "viewport": {
                "offset_x": document.layout.viewport.offset_x,
                "offset_y": document.layout.viewport.offset_y,
                "zoom": document.layout.viewport.zoom,
            },
            "metadata": thaw_json_object(document.layout.metadata),
        },
    }


def _check_project_text_size(text: str) -> None:
    if not isinstance(text, str):
        _fail("project document text must be a string")
    try:
        size = len(text.encode("utf-8"))
    except UnicodeError as error:
        raise ProjectFileError(f"invalid UTF-8 text: {error}") from error
    if size > MAX_PROJECT_FILE_BYTES:
        _fail(
            "project document exceeds the maximum size of "
            f"{MAX_PROJECT_FILE_BYTES // (1024 * 1024)} MiB"
        )


def dumps_project_document(document: ProjectDocument) -> str:
    data = document_to_data(document)
    _json_value(data, "document")
    text = json.dumps(data, indent=2, allow_nan=False) + "\n"
    _check_project_text_size(text)
    return text


def loads_project_document(text: str) -> ProjectDocument:
    _check_project_text_size(text)
    try:
        data = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=lambda value: _fail(
                f"invalid JSON numeric constant: {value}"
            ),
        )
    except json.JSONDecodeError as error:
        raise ProjectFileError(f"invalid JSON: {error.msg}") from error
    except ProjectFileError:
        raise
    except (RecursionError, ValueError) as error:
        raise ProjectFileError(f"invalid JSON: {error}") from error
    try:
        return document_from_data(data)
    except RecursionError as error:
        raise ProjectFileError(
            "project document exceeds the supported nesting depth"
        ) from error


def load_project_document(path: Path) -> ProjectDocument:
    try:
        with path.open("rb") as stream:
            payload = stream.read(MAX_PROJECT_FILE_BYTES + 1)
    except OSError as error:
        raise ProjectFileError(str(error)) from error
    if len(payload) > MAX_PROJECT_FILE_BYTES:
        _fail(
            "project document exceeds the maximum size of "
            f"{MAX_PROJECT_FILE_BYTES // (1024 * 1024)} MiB"
        )
    try:
        text = payload.decode("utf-8")
    except UnicodeError as error:
        raise ProjectFileError(str(error)) from error
    return loads_project_document(text)


def save_project_document(path: Path, document: ProjectDocument) -> None:
    """Atomically replace ``path`` without sharing or leaking temp files."""

    path = Path(path)
    text = dumps_project_document(document)
    descriptor: int | None = None
    temporary_path: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, raw_temporary_path = tempfile.mkstemp(
            prefix=f".{path.name}.",
            suffix=".g5proj.tmp",
            dir=path.parent,
            text=True,
        )
        temporary_path = Path(raw_temporary_path)
        with os.fdopen(
            descriptor, "w", encoding="utf-8", newline="\n"
        ) as stream:
            descriptor = None
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    except OSError as error:
        raise ProjectFileError(str(error)) from error
    finally:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass
            except OSError:
                # Preserve the original save result/error. A uniquely named,
                # ignored temp file is safer than masking the actionable cause.
                pass
