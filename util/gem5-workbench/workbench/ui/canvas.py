"""Pygame canvas adapter for the rendering-independent project document."""

import math
from dataclasses import dataclass
from enum import Enum

import pygame
from workbench.layout import (
    MAX_LAYOUT_COORDINATE,
    MAX_VIEWPORT_ZOOM,
    MIN_VIEWPORT_ZOOM,
    ComponentLayout,
    Position,
    ViewportLayout,
)
from workbench.model import (
    Component,
    ComponentId,
    Connection,
    Endpoint,
    ProjectMutationError,
)
from workbench.registry import (
    ComponentRegistry,
    ComponentSupport,
    PortDefinition,
    PortDirection,
    create_builtin_registry,
)
from workbench.state import WorkbenchState
from workbench.ui.base import Panel
from workbench.ui.theme import Theme
from workbench.ui.widgets import (
    draw_text,
    ellipsize,
    get_font,
)

NODE_SIZE = (150.0, 64.0)
NODE_HEADER_HEIGHT = 26.0
NODE_LABEL_TOP = 4.0
PORT_ROW_HEIGHT = 14.0
NODE_BOTTOM_PADDING = 8.0
GRID_SIZE = 24.0
MIN_ZOOM = MIN_VIEWPORT_ZOOM
MAX_ZOOM = MAX_VIEWPORT_ZOOM
MIN_PORT_AUTHORING_ZOOM = 0.2
CONNECTION_HIT_RADIUS = 7.0
PORT_RADIUS = 5
PORT_HIT_RADIUS = 9
NODE_EDGE_MARGIN = PORT_HIT_RADIUS + 1


class _HandleRole(Enum):
    SOURCE = "source"
    TARGET = "target"


@dataclass(frozen=True, slots=True)
class _PortHandle:
    endpoint: Endpoint
    definition: PortDefinition
    role: _HandleRole
    center: tuple[int, int]


class Canvas(Panel):
    def __init__(
        self,
        rect: pygame.Rect,
        theme: Theme,
        registry: ComponentRegistry | None = None,
    ) -> None:
        super().__init__(rect, theme)
        self.registry = registry or create_builtin_registry()
        self._dragging_component_id: ComponentId | None = None
        self._drag_offset = pygame.Vector2()
        self._connection_drag: _PortHandle | None = None
        self._connection_drag_mouse = pygame.Vector2()
        self._panning = False
        self._pan_last = pygame.Vector2()

    def set_rect(self, rect: pygame.Rect) -> None:
        super().set_rect(rect)
        self.cancel_interactions()

    def set_registry(self, registry: ComponentRegistry) -> None:
        self.registry = registry
        self.cancel_interactions()

    def cancel_interactions(self) -> None:
        self._dragging_component_id = None
        self._drag_offset.update(0, 0)
        self._cancel_connection_drag()
        self._panning = False
        self._pan_last.update(0, 0)

    @staticmethod
    def _layout_for(
        component_id: ComponentId, state: WorkbenchState
    ) -> ComponentLayout | None:
        return state.document.layout.components.get(component_id)

    @staticmethod
    def _viewport(state: WorkbenchState) -> ViewportLayout:
        return state.document.layout.viewport

    def _world_to_screen(
        self, position: Position, state: WorkbenchState
    ) -> pygame.Vector2:
        viewport = self._viewport(state)
        return pygame.Vector2(
            self.rect.x + (position.x - viewport.offset_x) * viewport.zoom,
            self.rect.y + (position.y - viewport.offset_y) * viewport.zoom,
        )

    def _screen_to_world(
        self, point: tuple[float, float], state: WorkbenchState
    ) -> Position:
        viewport = self._viewport(state)
        return Position(
            max(
                -MAX_LAYOUT_COORDINATE,
                min(
                    MAX_LAYOUT_COORDINATE,
                    viewport.offset_x
                    + (point[0] - self.rect.x) / viewport.zoom,
                ),
            ),
            max(
                -MAX_LAYOUT_COORDINATE,
                min(
                    MAX_LAYOUT_COORDINATE,
                    viewport.offset_y
                    + (point[1] - self.rect.y) / viewport.zoom,
                ),
            ),
        )

    def _component_rect(
        self, component: Component, state: WorkbenchState
    ) -> pygame.Rect:
        component_layout = self._layout_for(component.id, state)
        position = (
            component_layout.position if component_layout else Position()
        )
        screen = self._world_to_screen(position, state)
        zoom = self._viewport(state).zoom
        node_width, node_height = self._node_world_size(component.type_id)
        return pygame.Rect(
            round(screen.x),
            round(screen.y),
            max(1, round(node_width * zoom)),
            max(1, round(node_height * zoom)),
        )

    def _node_world_size(self, type_id: str) -> tuple[float, float]:
        definition = self.registry.get(type_id)
        maximum_ports = 0
        if definition is not None:
            maximum_ports = max(
                sum(
                    port.direction
                    in (PortDirection.INPUT, PortDirection.BIDIRECTIONAL)
                    for port in definition.ports.values()
                ),
                sum(
                    port.direction
                    in (PortDirection.OUTPUT, PortDirection.BIDIRECTIONAL)
                    for port in definition.ports.values()
                ),
            )
        required_height = (
            NODE_HEADER_HEIGHT
            + NODE_BOTTOM_PADDING
            + maximum_ports * PORT_ROW_HEIGHT
        )
        return NODE_SIZE[0], max(NODE_SIZE[1], required_height)

    def _component_at(
        self, point: tuple[int, int], state: WorkbenchState
    ) -> Component | None:
        components = tuple(state.document.project.components.values())
        for component in reversed(components):
            if self._component_rect(component, state).collidepoint(point):
                return component
        return None

    def _port_handles(
        self, component: Component, state: WorkbenchState
    ) -> tuple[_PortHandle, ...]:
        """Return role-specific handles for all registry-defined ports."""

        definition = self.registry.get(component.type_id)
        if (
            definition is None
            or self._viewport(state).zoom < MIN_PORT_AUTHORING_ZOOM
        ):
            return ()
        component_rect = self._component_rect(component, state)
        zoom = self._viewport(state).zoom
        header_height = min(
            component_rect.height,
            max(0, round(NODE_HEADER_HEIGHT * zoom)),
        )
        port_area_height = max(0, component_rect.height - header_height)
        handles: list[_PortHandle] = []
        role_directions = (
            (
                _HandleRole.TARGET,
                (PortDirection.INPUT, PortDirection.BIDIRECTIONAL),
                component_rect.left,
            ),
            (
                _HandleRole.SOURCE,
                (PortDirection.OUTPUT, PortDirection.BIDIRECTIONAL),
                component_rect.right,
            ),
        )
        for role, directions, x_position in role_directions:
            ports = tuple(
                port
                for port in definition.ports.values()
                if port.direction in directions
            )
            for index, port in enumerate(ports):
                y_position = round(
                    component_rect.top
                    + header_height
                    + port_area_height * (index + 1) / (len(ports) + 1)
                )
                handles.append(
                    _PortHandle(
                        Endpoint(component.id, port.id),
                        port,
                        role,
                        (x_position, y_position),
                    )
                )
        return tuple(handles)

    def _port_handle_for(
        self,
        endpoint: Endpoint,
        role: _HandleRole,
        state: WorkbenchState,
    ) -> _PortHandle | None:
        component = state.document.project.components.get(
            endpoint.component_id
        )
        if component is None:
            return None
        for handle in self._port_handles(component, state):
            if (
                handle.endpoint.port_id == endpoint.port_id
                and handle.role is role
            ):
                return handle
        return None

    def _port_handle_at(
        self, point: tuple[int, int], state: WorkbenchState
    ) -> _PortHandle | None:
        mouse = pygame.Vector2(point)
        components = tuple(state.document.project.components.values())
        for component in reversed(components):
            candidates = (
                (mouse.distance_to(handle.center), handle)
                for handle in self._port_handles(component, state)
            )
            nearby = tuple(
                candidate
                for candidate in candidates
                if candidate[0] <= PORT_HIT_RADIUS
            )
            if nearby:
                return min(nearby, key=lambda candidate: candidate[0])[1]
        return None

    @staticmethod
    def _port_usage(
        component_id: ComponentId,
        port_id: str,
        state: WorkbenchState,
    ) -> int:
        count = 0
        for connection in state.document.project.connections.values():
            for endpoint in (connection.source, connection.target):
                if (
                    endpoint.component_id == component_id
                    and endpoint.port_id == port_id
                ):
                    count += 1
        return count

    def _port_has_capacity(
        self, handle: _PortHandle, state: WorkbenchState
    ) -> bool:
        maximum = handle.definition.maximum_connections
        return (
            maximum is None
            or self._port_usage(
                handle.endpoint.component_id,
                handle.endpoint.port_id,
                state,
            )
            < maximum
        )

    @staticmethod
    def _ordered_connection_handles(
        first: _PortHandle, second: _PortHandle
    ) -> tuple[_PortHandle, _PortHandle] | None:
        if (
            first.role is _HandleRole.SOURCE
            and second.role is _HandleRole.TARGET
        ):
            return first, second
        if (
            first.role is _HandleRole.TARGET
            and second.role is _HandleRole.SOURCE
        ):
            return second, first
        return None

    def _can_connect(
        self,
        first: _PortHandle,
        second: _PortHandle,
        state: WorkbenchState,
    ) -> bool:
        ordered = self._ordered_connection_handles(first, second)
        if ordered is None:
            return False
        source, target = ordered
        if (
            source.endpoint.component_id == target.endpoint.component_id
            and source.endpoint.port_id == target.endpoint.port_id
        ):
            return False
        if source.definition.vector or target.definition.vector:
            return False
        if source.definition.interface != target.definition.interface:
            return False

        required_capacity: dict[tuple[ComponentId, str], int] = {}
        definitions: dict[tuple[ComponentId, str], PortDefinition] = {}
        for handle in (source, target):
            key = (handle.endpoint.component_id, handle.endpoint.port_id)
            required_capacity[key] = required_capacity.get(key, 0) + 1
            definitions[key] = handle.definition
        for (component_id, port_id), additional in required_capacity.items():
            maximum = definitions[(component_id, port_id)].maximum_connections
            if maximum is not None and (
                self._port_usage(component_id, port_id, state) + additional
                > maximum
            ):
                return False
        return True

    def _valid_connection_targets(
        self, state: WorkbenchState
    ) -> tuple[_PortHandle, ...]:
        start = self._connection_drag
        if start is None:
            return ()
        targets: list[_PortHandle] = []
        for component in state.document.project.components.values():
            for handle in self._port_handles(component, state):
                if self._can_connect(start, handle, state):
                    targets.append(handle)
        return tuple(targets)

    def _cancel_connection_drag(self) -> None:
        self._connection_drag = None
        self._connection_drag_mouse.update(0, 0)

    def _finish_connection_drag(
        self, point: tuple[int, int], state: WorkbenchState
    ) -> None:
        stale_start = self._connection_drag
        start = (
            self._port_handle_for(
                stale_start.endpoint,
                stale_start.role,
                state,
            )
            if stale_start is not None
            else None
        )
        target = self._port_handle_at(point, state)
        if (
            start is None
            or target is None
            or not self._can_connect(start, target, state)
        ):
            self._cancel_connection_drag()
            state.status_message = "Connection cancelled"
            return
        ordered = self._ordered_connection_handles(start, target)
        if ordered is None:
            self._cancel_connection_drag()
            state.status_message = "Connection cancelled"
            return
        source, destination = ordered
        connection = Connection(source.endpoint, destination.endpoint)
        try:
            state.document.project.add_connection(connection)
        except (ProjectMutationError, ValueError) as error:
            self._cancel_connection_drag()
            state.status_message = f"Connection cancelled: {error}"
            return
        state.selected_component_id = None
        state.selected_connection_id = connection.id
        state.selected_type_id = None
        state.diagnostics.clear()
        state.status_message = (
            f"Connected {source.endpoint.port_id} to "
            f"{destination.endpoint.port_id}"
        )
        self._cancel_connection_drag()

    def _connection_segment(
        self, connection: Connection, state: WorkbenchState
    ) -> tuple[pygame.Vector2, pygame.Vector2] | None:
        components = state.document.project.components
        source = components.get(connection.source.component_id)
        target = components.get(connection.target.component_id)
        if source is None or target is None:
            return None
        if (
            self._layout_for(source.id, state) is None
            or self._layout_for(target.id, state) is None
        ):
            return None
        source_handle = self._port_handle_for(
            connection.source, _HandleRole.SOURCE, state
        )
        target_handle = self._port_handle_for(
            connection.target, _HandleRole.TARGET, state
        )
        return (
            pygame.Vector2(
                source_handle.center
                if source_handle
                else self._component_rect(source, state).midright
            ),
            pygame.Vector2(
                target_handle.center
                if target_handle
                else self._component_rect(target, state).midleft
            ),
        )

    @staticmethod
    def _distance_to_segment(
        point: pygame.Vector2,
        start: pygame.Vector2,
        end: pygame.Vector2,
    ) -> float:
        segment = end - start
        length_squared = segment.length_squared()
        if length_squared == 0:
            return point.distance_to(start)
        projection = max(
            0.0,
            min(1.0, (point - start).dot(segment) / length_squared),
        )
        return point.distance_to(start + segment * projection)

    def _connection_at(
        self, point: tuple[int, int], state: WorkbenchState
    ) -> Connection | None:
        mouse = pygame.Vector2(point)
        connections = tuple(state.document.project.connections.values())
        for connection in reversed(connections):
            segment = self._connection_segment(connection, state)
            if (
                segment
                and self._distance_to_segment(mouse, *segment)
                <= CONNECTION_HIT_RADIUS
            ):
                return connection
        return None

    def repair_layout(self, state: WorkbenchState) -> list[ComponentId]:
        return state.document.repair_layout()

    def all_components_visible(self, state: WorkbenchState) -> bool:
        components = state.document.project.components.values()
        return all(
            self.rect.contains(self._component_rect(component, state))
            for component in components
        )

    def fit_to_components(self, state: WorkbenchState) -> None:
        """Adjust only the viewport so every laid-out component is reachable."""

        self.repair_layout(state)
        components = tuple(state.document.project.components.values())
        if not components or self.rect.width <= 0 or self.rect.height <= 0:
            return
        positioned = [
            (
                state.document.layout.components[component.id].position,
                self._node_world_size(component.type_id),
            )
            for component in components
        ]
        minimum_x = min(position.x for position, _ in positioned)
        minimum_y = min(position.y for position, _ in positioned)
        maximum_x = max(position.x + size[0] for position, size in positioned)
        maximum_y = max(position.y + size[1] for position, size in positioned)
        padding = 32.0
        world_width = max(1.0, maximum_x - minimum_x)
        world_height = max(1.0, maximum_y - minimum_y)
        zoom = min(
            1.0,
            max(
                MIN_ZOOM,
                min(
                    max(1.0, self.rect.width - padding * 2) / world_width,
                    max(1.0, self.rect.height - padding * 2) / world_height,
                ),
            ),
        )
        visible_width = self.rect.width / zoom
        visible_height = self.rect.height / zoom
        offset_x = minimum_x - (visible_width - world_width) / 2
        offset_y = minimum_y - (visible_height - world_height) / 2
        state.document.layout.set_viewport(
            ViewportLayout(
                max(
                    -MAX_LAYOUT_COORDINATE,
                    min(MAX_LAYOUT_COORDINATE, offset_x),
                ),
                max(
                    -MAX_LAYOUT_COORDINATE,
                    min(MAX_LAYOUT_COORDINATE, offset_y),
                ),
                zoom,
            )
        )

    def ensure_components_visible(self, state: WorkbenchState) -> None:
        self.repair_layout(state)
        if not self.all_components_visible(state):
            self.fit_to_components(state)

    def constrain_components(self, state: WorkbenchState) -> None:
        """Backward-compatible name for viewport-based visibility fitting."""

        self.fit_to_components(state)

    def _add_component(
        self,
        type_id: str,
        point: tuple[int, int],
        state: WorkbenchState,
    ) -> bool:
        zoom = self._viewport(state).zoom
        node_width, node_height = self._node_world_size(type_id)
        node_size = (
            max(1, round(node_width * zoom)),
            max(1, round(node_height * zoom)),
        )
        top_left = self._clamp_component_top_left(
            (
                point[0] - node_size[0] / 2,
                point[1] - node_size[1] / 2,
            ),
            node_size,
        )
        position = self._screen_to_world(top_left, state)
        definition = self.registry.get(type_id)
        if (
            definition is None
            or definition.support is not ComponentSupport.SUPPORTED
        ):
            state.status_message = (
                definition.support_reason
                if definition is not None and definition.support_reason
                else "This catalog entry cannot be placed"
            )
            return False
        parameters = (
            {
                parameter.id: parameter.default
                for parameter in definition.parameters.values()
                if parameter.has_default
            }
            if definition
            else {}
        )
        component = state.document.add_component(
            type_id, position, parameters=parameters
        )
        state.selected_component_id = component.id
        state.selected_connection_id = None
        state.diagnostics.clear()
        state.status_message = f"Added {type_id}"
        return True

    def _clamp_component_top_left(
        self,
        point: tuple[float, float],
        size: tuple[int, int],
    ) -> tuple[float, float]:
        """Keep a directly manipulated node reachable inside the Canvas."""

        right_margin = min(
            NODE_EDGE_MARGIN,
            max(0.0, self.rect.width - size[0]),
        )
        maximum_x = max(
            self.rect.left,
            self.rect.right - size[0] - right_margin,
        )
        maximum_y = max(self.rect.top, self.rect.bottom - size[1])
        return (
            max(self.rect.left, min(maximum_x, point[0])),
            max(self.rect.top, min(maximum_y, point[1])),
        )

    def _zoom_at_mouse(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        mouse = event.pos if hasattr(event, "pos") else pygame.mouse.get_pos()
        if not self.rect.collidepoint(mouse):
            return False
        try:
            wheel_delta = float(event.y)
        except (AttributeError, TypeError, ValueError, OverflowError):
            return False
        if not math.isfinite(wheel_delta):
            return False
        # SDL normally supplies a small integer, but synthesized events and
        # unusual pointing devices can report much larger values. Beyond this
        # range the result is clamped anyway, so avoid overflowing ``pow``.
        wheel_delta = max(-100.0, min(100.0, wheel_delta))
        viewport = self._viewport(state)
        world_at_mouse = self._screen_to_world(mouse, state)
        zoom = max(
            MIN_ZOOM,
            min(MAX_ZOOM, viewport.zoom * math.pow(1.12, wheel_delta)),
        )
        offset_x = world_at_mouse.x - (mouse[0] - self.rect.x) / zoom
        offset_y = world_at_mouse.y - (mouse[1] - self.rect.y) / zoom
        state.document.layout.set_viewport(
            ViewportLayout(
                max(
                    -MAX_LAYOUT_COORDINATE,
                    min(MAX_LAYOUT_COORDINATE, offset_x),
                ),
                max(
                    -MAX_LAYOUT_COORDINATE,
                    min(MAX_LAYOUT_COORDINATE, offset_y),
                ),
                zoom,
            )
        )
        return True

    def _pan(self, point: tuple[int, int], state: WorkbenchState) -> None:
        current = pygame.Vector2(point)
        delta = current - self._pan_last
        self._pan_last = current
        viewport = self._viewport(state)
        offset_x = viewport.offset_x - delta.x / viewport.zoom
        offset_y = viewport.offset_y - delta.y / viewport.zoom
        state.document.layout.set_viewport(
            ViewportLayout(
                max(
                    -MAX_LAYOUT_COORDINATE,
                    min(MAX_LAYOUT_COORDINATE, offset_x),
                ),
                max(
                    -MAX_LAYOUT_COORDINATE,
                    min(MAX_LAYOUT_COORDINATE, offset_y),
                ),
                viewport.zoom,
            )
        )

    def handle_event(
        self, event: pygame.event.Event, state: WorkbenchState
    ) -> bool:
        if event.type == pygame.MOUSEWHEEL:
            return self._zoom_at_mouse(event, state)
        if event.type == pygame.KEYDOWN:
            if event.key == pygame.K_ESCAPE:
                if self._connection_drag is not None:
                    self._cancel_connection_drag()
                    state.status_message = "Connection cancelled"
                    return True
                if self._dragging_component_id is not None or self._panning:
                    self.cancel_interactions()
                    state.status_message = "Canvas interaction stopped"
                    return True
                if state.selected_type_id is not None:
                    state.selected_type_id = None
                    state.status_message = "Placement tool cleared"
                    return True
                if (
                    state.selected_component_id is not None
                    or state.selected_connection_id is not None
                ):
                    state.selected_component_id = None
                    state.selected_connection_id = None
                    state.status_message = "Selection cleared"
                    return True
                state.status_message = "Ready"
                return True
            if event.key in (pygame.K_DELETE, pygame.K_BACKSPACE):
                had_interaction = (
                    self._connection_drag is not None
                    or self._dragging_component_id is not None
                    or self._panning
                )
                self.cancel_interactions()
                if state.selected_connection_id is not None:
                    connection_id = state.selected_connection_id
                    if connection_id in state.document.project.connections:
                        state.document.project.remove_connection(connection_id)
                        state.status_message = "Removed connection"
                    else:
                        state.status_message = (
                            "Connection selection no longer exists"
                        )
                    state.selected_connection_id = None
                    state.diagnostics.clear()
                    return True
                component_id = state.selected_component_id
                if component_id is not None:
                    if component_id in state.document.project.components:
                        component = state.document.remove_component(
                            component_id
                        )
                        state.status_message = f"Removed {component.type_id}"
                    else:
                        state.status_message = (
                            "Component selection no longer exists"
                        )
                    state.selected_component_id = None
                    state.diagnostics.clear()
                    return True
                if had_interaction:
                    state.status_message = "Canvas interaction stopped"
                    return True

        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 2:
            if not self.rect.collidepoint(event.pos):
                return False
            self._dragging_component_id = None
            self._cancel_connection_drag()
            self._panning = True
            self._pan_last.update(event.pos)
            return True

        if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            if not self.rect.collidepoint(event.pos):
                return False
            self._panning = False
            self._cancel_connection_drag()
            self.repair_layout(state)
            port_handle = self._port_handle_at(event.pos, state)
            if port_handle is not None:
                self._dragging_component_id = None
                state.selected_component_id = port_handle.endpoint.component_id
                state.selected_connection_id = None
                state.selected_type_id = None
                if port_handle.definition.vector:
                    state.status_message = (
                        "Vector-port connections are not supported yet"
                    )
                elif not self._port_has_capacity(port_handle, state):
                    state.status_message = (
                        f"Port {port_handle.endpoint.port_id} is already full"
                    )
                else:
                    self._connection_drag = port_handle
                    self._connection_drag_mouse.update(port_handle.center)
                    state.status_message = (
                        f"Connecting {port_handle.endpoint.port_id}"
                    )
                return True
            selected = self._component_at(event.pos, state)
            if selected is not None:
                state.selected_component_id = selected.id
                state.selected_connection_id = None
                state.selected_type_id = None
                self._dragging_component_id = selected.id
                component_rect = self._component_rect(selected, state)
                self._drag_offset = pygame.Vector2(
                    event.pos[0] - component_rect.x,
                    event.pos[1] - component_rect.y,
                )
                state.status_message = f"Selected {selected.type_id}"
            else:
                connection = self._connection_at(event.pos, state)
                if connection is not None:
                    state.selected_connection_id = connection.id
                    state.selected_component_id = None
                    state.selected_type_id = None
                    state.status_message = (
                        f"Selected connection {connection.id}"
                    )
                elif state.selected_type_id:
                    placed = self._add_component(
                        state.selected_type_id, event.pos, state
                    )
                    if placed:
                        state.selected_type_id = None
                else:
                    state.selected_component_id = None
                    state.selected_connection_id = None
                    state.status_message = "Ready"
            return True

        if event.type == pygame.MOUSEMOTION and self._panning:
            self._pan(event.pos, state)
            return True

        if (
            event.type == pygame.MOUSEMOTION
            and self._connection_drag is not None
        ):
            self._connection_drag_mouse.update(event.pos)
            return True

        if event.type == pygame.MOUSEMOTION and self._dragging_component_id:
            component_layout = self._layout_for(
                self._dragging_component_id, state
            )
            if component_layout is not None:
                top_left = (
                    event.pos[0] - self._drag_offset.x,
                    event.pos[1] - self._drag_offset.y,
                )
                component = state.document.project.components.get(
                    self._dragging_component_id
                )
                if component is None:
                    self._dragging_component_id = None
                    return True
                component_rect = self._component_rect(component, state)
                top_left = self._clamp_component_top_left(
                    top_left, component_rect.size
                )
                state.document.layout.move_component(
                    self._dragging_component_id,
                    self._screen_to_world(top_left, state),
                )
                return True
            self._dragging_component_id = None

        if event.type == pygame.MOUSEBUTTONUP and event.button == 1:
            if self._connection_drag is not None:
                self._finish_connection_drag(event.pos, state)
                return True
            if self._dragging_component_id is not None:
                self._dragging_component_id = None
                return True
        if event.type == pygame.MOUSEBUTTONUP and event.button == 2:
            if self._panning:
                self._panning = False
                return True
        return False

    def _draw_grid(
        self, surface: pygame.Surface, state: WorkbenchState
    ) -> None:
        viewport = self._viewport(state)
        step = GRID_SIZE * viewport.zoom
        while step < 10:
            step *= 2
        start_x = self.rect.x + (-viewport.offset_x * viewport.zoom) % step
        start_y = self.rect.y + (-viewport.offset_y * viewport.zoom) % step
        x = start_x
        while x < self.rect.right:
            pygame.draw.line(
                surface,
                self.theme.grid,
                (round(x), self.rect.top),
                (round(x), self.rect.bottom),
            )
            x += step
        y = start_y
        while y < self.rect.bottom:
            pygame.draw.line(
                surface,
                self.theme.grid,
                (self.rect.left, round(y)),
                (self.rect.right, round(y)),
            )
            y += step

    def _draw_connections(
        self, surface: pygame.Surface, state: WorkbenchState
    ) -> None:
        for connection in state.document.project.connections.values():
            segment = self._connection_segment(connection, state)
            if segment is None:
                continue
            color = (
                self.theme.accent
                if connection.id == state.selected_connection_id
                else self.theme.muted_text
            )
            pygame.draw.line(surface, color, *segment, width=2)
        if self._connection_drag is not None:
            pygame.draw.line(
                surface,
                self.theme.accent_hover,
                self._connection_drag.center,
                self._connection_drag_mouse,
                width=2,
            )

    def _draw_port_handles(
        self,
        surface: pygame.Surface,
        component: Component,
        state: WorkbenchState,
        valid_targets: set[tuple[ComponentId, str, _HandleRole]],
    ) -> None:
        active = self._connection_drag
        zoom = self._viewport(state).zoom
        radius = max(3, min(7, round(PORT_RADIUS * zoom)))
        port_label_size = max(9, round(10 * min(zoom, 1.2)))
        for handle in self._port_handles(component, state):
            key = (
                handle.endpoint.component_id,
                handle.endpoint.port_id,
                handle.role,
            )
            is_active = active is not None and key == (
                active.endpoint.component_id,
                active.endpoint.port_id,
                active.role,
            )
            if key in valid_targets:
                pygame.draw.circle(
                    surface,
                    self.theme.accent_hover,
                    handle.center,
                    radius + 3,
                    width=2,
                )
            color = (
                self.theme.muted_text
                if handle.definition.vector
                else self.theme.accent if is_active else self.theme.node_border
            )
            pygame.draw.circle(surface, color, handle.center, radius)
            pygame.draw.circle(
                surface,
                self.theme.canvas_background,
                handle.center,
                radius,
                width=1,
            )
            if zoom >= 0.65:
                if handle.role is _HandleRole.TARGET:
                    label_position = (
                        handle.center[0] + radius + 3,
                        handle.center[1],
                    )
                    anchor = "midleft"
                else:
                    label_position = (
                        handle.center[0] - radius - 3,
                        handle.center[1],
                    )
                    anchor = "midright"
                draw_text(
                    surface,
                    handle.endpoint.port_id,
                    label_position,
                    color=self.theme.muted_text,
                    size=port_label_size,
                    anchor=anchor,
                )

    def draw(self, surface: pygame.Surface, state: WorkbenchState) -> None:
        if self.rect.width <= 0 or self.rect.height <= 0:
            return
        pygame.draw.rect(surface, self.theme.canvas_background, self.rect)
        previous_clip = surface.get_clip()
        surface.set_clip(self.rect)
        try:
            self._draw_grid(surface, state)
            self._draw_connections(surface, state)
            valid_targets = {
                (
                    handle.endpoint.component_id,
                    handle.endpoint.port_id,
                    handle.role,
                )
                for handle in self._valid_connection_targets(state)
            }
            zoom = self._viewport(state).zoom
            label_size = max(9, round(16 * min(zoom, 1.2)))
            label_font = get_font(label_size, bold=True)
            for component in state.document.project.components.values():
                component_rect = self._component_rect(component, state)
                pygame.draw.rect(
                    surface,
                    self.theme.node_background,
                    component_rect,
                    border_radius=8,
                )
                selected = component.id == state.selected_component_id
                border = (
                    self.theme.accent if selected else self.theme.node_border
                )
                pygame.draw.rect(
                    surface,
                    border,
                    component_rect,
                    width=2 if selected else 1,
                    border_radius=8,
                )
                definition = self.registry.get(component.type_id)
                label = (
                    definition.display_name
                    if definition is not None
                    else component.type_id
                )
                has_ports = definition is not None and bool(definition.ports)
                header_height = max(0, round(NODE_HEADER_HEIGHT * zoom))
                title_top = component_rect.top + max(
                    1, round(NODE_LABEL_TOP * min(zoom, 1.0))
                )
                if not has_ports or (
                    header_height >= label_font.get_height() + 4
                ):
                    position = (
                        (component_rect.centerx, title_top)
                        if has_ports
                        else component_rect.center
                    )
                    draw_text(
                        surface,
                        ellipsize(
                            label, label_font, component_rect.width - 24
                        ),
                        position,
                        color=self.theme.text,
                        size=label_size,
                        bold=True,
                        anchor="midtop" if has_ports else "center",
                    )
                self._draw_port_handles(
                    surface, component, state, valid_targets
                )
            if not state.document.project.components:
                draw_text(
                    surface,
                    "Choose a component, then place it here",
                    self.rect.center,
                    color=self.theme.muted_text,
                    size=17,
                    anchor="center",
                )
        finally:
            surface.set_clip(previous_clip)
