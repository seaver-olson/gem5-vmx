"""Registry-backed component, parameter, and port validation."""

from collections import Counter

from workbench.document import ProjectDocument
from workbench.model.values import JsonValue
from workbench.registry import (
    ComponentRegistry,
    ParameterType,
    PortDefinition,
    PortDirection,
)
from workbench.validation.diagnostics import (
    Diagnostic,
    DiagnosticLayer,
    DiagnosticSeverity,
)


def _matches_parameter_type(value: JsonValue, expected: ParameterType) -> bool:
    if expected is ParameterType.BOOLEAN:
        return isinstance(value, bool)
    if expected is ParameterType.INTEGER:
        return isinstance(value, int) and not isinstance(value, bool)
    if expected is ParameterType.NUMBER:
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, str)


def _allows_source(port: PortDefinition) -> bool:
    return port.direction in (
        PortDirection.OUTPUT,
        PortDirection.BIDIRECTIONAL,
    )


def _allows_target(port: PortDefinition) -> bool:
    return port.direction in (PortDirection.INPUT, PortDirection.BIDIRECTIONAL)


def validate_registry(
    document: ProjectDocument,
    registry: ComponentRegistry,
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    project = document.project
    definitions = {
        component_id: registry.get(component.type_id)
        for component_id, component in project.components.items()
    }
    for component_id, component in project.components.items():
        definition = definitions[component_id]
        if definition is None:
            diagnostics.append(
                Diagnostic(
                    "registry.unknown_component_type",
                    f"Unknown component type: {component.type_id}",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.REGISTRY,
                    component_id=component_id,
                    field="type_id",
                )
            )
            continue
        for parameter_id, parameter in definition.parameters.items():
            if parameter.required and parameter_id not in component.parameters:
                diagnostics.append(
                    Diagnostic(
                        "registry.required_parameter",
                        f"Required parameter is missing: {parameter_id}",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"parameters.{parameter_id}",
                    )
                )
        for parameter_id, value in component.parameters.items():
            parameter = definition.parameters.get(parameter_id)
            if parameter is None:
                diagnostics.append(
                    Diagnostic(
                        "registry.unknown_parameter",
                        f"Unknown parameter: {parameter_id}",
                        DiagnosticSeverity.WARNING,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"parameters.{parameter_id}",
                    )
                )
            elif not (
                (value is None and parameter.nullable)
                or _matches_parameter_type(value, parameter.value_type)
            ):
                diagnostics.append(
                    Diagnostic(
                        "registry.parameter_type",
                        f"Parameter {parameter_id} must be {parameter.value_type.value}",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"parameters.{parameter_id}",
                    )
                )
            elif (
                value is not None
                and parameter.choices
                and value not in parameter.choices
            ):
                diagnostics.append(
                    Diagnostic(
                        "registry.parameter_choice",
                        f"Parameter {parameter_id} is not an allowed choice",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"parameters.{parameter_id}",
                    )
                )
            elif (
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                and parameter.minimum is not None
                and value < parameter.minimum
            ):
                diagnostics.append(
                    Diagnostic(
                        "registry.parameter_minimum",
                        f"Parameter {parameter_id} must be at least {parameter.minimum}",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"parameters.{parameter_id}",
                    )
                )
            elif (
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                and parameter.maximum is not None
                and value > parameter.maximum
            ):
                diagnostics.append(
                    Diagnostic(
                        "registry.parameter_maximum",
                        f"Parameter {parameter_id} must be at most {parameter.maximum}",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"parameters.{parameter_id}",
                    )
                )

    port_usage: Counter[tuple[object, str]] = Counter()
    slot_usage: Counter[tuple[object, str, int]] = Counter()
    for connection in project.connections.values():
        source_definition = definitions.get(connection.source.component_id)
        target_definition = definitions.get(connection.target.component_id)
        source_port = (
            source_definition.ports.get(connection.source.port_id)
            if source_definition
            else None
        )
        target_port = (
            target_definition.ports.get(connection.target.port_id)
            if target_definition
            else None
        )
        for field, endpoint, port in (
            ("source", connection.source, source_port),
            ("target", connection.target, target_port),
        ):
            if (
                definitions.get(endpoint.component_id) is not None
                and port is None
            ):
                diagnostics.append(
                    Diagnostic(
                        "registry.unknown_port",
                        f"Unknown {field} port: {endpoint.port_id}",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=endpoint.component_id,
                        connection_id=connection.id,
                        field=f"{field}.port_id",
                    )
                )
            if port is not None:
                port_usage[(endpoint.component_id, endpoint.port_id)] += 1
                if endpoint.slot is not None and not port.vector:
                    diagnostics.append(
                        Diagnostic(
                            "registry.non_vector_slot",
                            "A slot was supplied for a non-vector port",
                            DiagnosticSeverity.ERROR,
                            DiagnosticLayer.REGISTRY,
                            component_id=endpoint.component_id,
                            connection_id=connection.id,
                            field=f"{field}.slot",
                        )
                    )
                elif endpoint.slot is None and port.vector:
                    diagnostics.append(
                        Diagnostic(
                            "registry.unresolved_vector_slot",
                            "Vector-port connections require an explicit slot",
                            DiagnosticSeverity.ERROR,
                            DiagnosticLayer.REGISTRY,
                            component_id=endpoint.component_id,
                            connection_id=connection.id,
                            field=f"{field}.slot",
                        )
                    )
                elif endpoint.slot is not None:
                    slot_usage[
                        (
                            endpoint.component_id,
                            endpoint.port_id,
                            endpoint.slot,
                        )
                    ] += 1
                    if (
                        port.maximum_connections is not None
                        and endpoint.slot >= port.maximum_connections
                    ):
                        diagnostics.append(
                            Diagnostic(
                                "registry.vector_slot_range",
                                "Vector-port slot must be below "
                                f"{port.maximum_connections}",
                                DiagnosticSeverity.ERROR,
                                DiagnosticLayer.REGISTRY,
                                component_id=endpoint.component_id,
                                connection_id=connection.id,
                                field=f"{field}.slot",
                            )
                        )
        if source_port and not _allows_source(source_port):
            diagnostics.append(
                Diagnostic(
                    "registry.source_direction",
                    "Source port does not allow outgoing connections",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.REGISTRY,
                    connection_id=connection.id,
                    field="source.port_id",
                )
            )
        if target_port and not _allows_target(target_port):
            diagnostics.append(
                Diagnostic(
                    "registry.target_direction",
                    "Target port does not allow incoming connections",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.REGISTRY,
                    connection_id=connection.id,
                    field="target.port_id",
                )
            )
        if (
            source_port
            and target_port
            and source_port.interface != target_port.interface
        ):
            diagnostics.append(
                Diagnostic(
                    "registry.interface_mismatch",
                    "Connected ports use incompatible interfaces",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.REGISTRY,
                    connection_id=connection.id,
                )
            )

    for component_id, definition in definitions.items():
        if definition is None:
            continue
        for port_id, port in definition.ports.items():
            count = port_usage[(component_id, port_id)]
            if (
                port.maximum_connections is not None
                and count > port.maximum_connections
            ):
                diagnostics.append(
                    Diagnostic(
                        "registry.port_cardinality",
                        f"Port {port_id} allows at most {port.maximum_connections} connections",
                        DiagnosticSeverity.ERROR,
                        DiagnosticLayer.REGISTRY,
                        component_id=component_id,
                        field=f"ports.{port_id}",
                    )
                )
    for (component_id, port_id, slot), count in slot_usage.items():
        if count > 1:
            diagnostics.append(
                Diagnostic(
                    "registry.duplicate_vector_slot",
                    f"Vector port {port_id} slot {slot} is used {count} times",
                    DiagnosticSeverity.ERROR,
                    DiagnosticLayer.REGISTRY,
                    component_id=component_id,
                    field=f"ports.{port_id}[{slot}]",
                )
            )
    return diagnostics
