"""Translation-readiness checks independent of the host gem5 installation."""

from collections import defaultdict

from workbench.document import ProjectDocument
from workbench.registry import (
    SE_WORKLOAD_TYPE_ID,
    SIMPLE_BOARD_TYPE_ID,
    ComponentRegistry,
    ComponentSupport,
)
from workbench.validation.diagnostics import (
    Diagnostic,
    DiagnosticLayer,
    DiagnosticSeverity,
)


def _error(
    code: str,
    message: str,
    *,
    component_id=None,
    connection_id=None,
    field: str | None = None,
) -> Diagnostic:
    return Diagnostic(
        code,
        message,
        DiagnosticSeverity.ERROR,
        DiagnosticLayer.EXECUTION,
        component_id=component_id,
        connection_id=connection_id,
        field=field,
    )


def _cycle_nodes(document: ProjectDocument) -> set[object]:
    component_ids = set(document.project.components)
    graph: dict[object, list[object]] = defaultdict(list)
    reverse_graph: dict[object, list[object]] = defaultdict(list)
    for connection in document.project.connections.values():
        if (
            connection.source.component_id in component_ids
            and connection.target.component_id in component_ids
        ):
            source = connection.source.component_id
            target = connection.target.component_id
            graph[source].append(target)
            reverse_graph[target].append(source)

    # Kosaraju's algorithm is iterative here because a valid project may have
    # a dependency chain longer than Python's recursion limit. A back-edge-only
    # DFS is insufficient: it can omit vertices in a strongly connected
    # component when a later branch rejoins an already visited vertex.
    visited: set[object] = set()
    finishing_order: list[object] = []
    for component_id in document.project.components:
        if component_id in visited:
            continue
        visited.add(component_id)
        stack: list[tuple[object, int]] = [(component_id, 0)]
        while stack:
            node, neighbor_index = stack[-1]
            neighbors = graph[node]
            if neighbor_index >= len(neighbors):
                stack.pop()
                finishing_order.append(node)
                continue
            neighbor = neighbors[neighbor_index]
            stack[-1] = (node, neighbor_index + 1)
            if neighbor not in visited:
                visited.add(neighbor)
                stack.append((neighbor, 0))

    cycles: set[object] = set()
    assigned: set[object] = set()
    for component_id in reversed(finishing_order):
        if component_id in assigned:
            continue
        assigned.add(component_id)
        strongly_connected: list[object] = []
        stack = [component_id]
        while stack:
            node = stack.pop()
            strongly_connected.append(node)
            for neighbor in reverse_graph[node]:
                if neighbor not in assigned:
                    assigned.add(neighbor)
                    stack.append(neighbor)
        if len(strongly_connected) > 1 or component_id in graph[component_id]:
            cycles.update(strongly_connected)
    return cycles


def validate_execution_readiness(
    document: ProjectDocument,
    registry: ComponentRegistry,
) -> list[Diagnostic]:
    """Validate whether the project can be translated by the current bridge."""

    diagnostics: list[Diagnostic] = []
    components = document.project.components
    board_ids = [
        component.id
        for component in components.values()
        if component.type_id == SIMPLE_BOARD_TYPE_ID
    ]
    if len(board_ids) != 1:
        diagnostics.append(
            _error(
                "execution.board_count",
                "A runnable project requires exactly one Simple Board",
            )
        )

    for component in components.values():
        definition = registry.get(component.type_id)
        if (
            definition is not None
            and definition.support is not ComponentSupport.SUPPORTED
        ):
            diagnostics.append(
                _error(
                    "execution.unsupported_component",
                    definition.support_reason
                    or "Component is not executable with this catalog",
                    component_id=component.id,
                )
            )
        if component.type_id == SE_WORKLOAD_TYPE_ID:
            source_kind = component.parameters.get("source_kind")
            if source_kind == "local":
                if not component.parameters.get("local_path"):
                    diagnostics.append(
                        _error(
                            "execution.local_path",
                            "Local workloads require a binary path",
                            component_id=component.id,
                            field="parameters.local_path",
                        )
                    )
            elif source_kind == "resource":
                if not component.parameters.get("resource_id"):
                    diagnostics.append(
                        _error(
                            "execution.resource_id",
                            "Resource workloads require a resource ID",
                            component_id=component.id,
                            field="parameters.resource_id",
                        )
                    )
            else:
                diagnostics.append(
                    _error(
                        "execution.workload_source",
                        "Workload source must be local or resource",
                        component_id=component.id,
                        field="parameters.source_kind",
                    )
                )

    for connection in document.project.connections.values():
        if connection.parameters:
            diagnostics.append(
                _error(
                    "execution.connection_parameters",
                    "The current execution bridge does not support "
                    "connection parameters",
                    connection_id=connection.id,
                    field="parameters",
                )
            )

    cycle_nodes = _cycle_nodes(document)
    for component_id in components:
        if component_id in cycle_nodes:
            diagnostics.append(
                _error(
                    "execution.dependency_cycle",
                    "Component participates in a dependency cycle",
                    component_id=component_id,
                )
            )

    if len(board_ids) == 1:
        board_id = board_ids[0]
        incoming: dict[object, set[object]] = defaultdict(set)
        for connection in document.project.connections.values():
            incoming[connection.target.component_id].add(
                connection.source.component_id
            )
        reachable = {board_id}
        pending = [board_id]
        while pending:
            target = pending.pop()
            for source in incoming[target]:
                if source not in reachable:
                    reachable.add(source)
                    pending.append(source)
        for component_id in components:
            if component_id not in reachable:
                diagnostics.append(
                    _error(
                        "execution.orphan_component",
                        "Component is not connected to the executable board",
                        component_id=component_id,
                    )
                )
    return diagnostics
