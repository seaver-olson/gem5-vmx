"""Convert automatic catalog entries into palette-facing definitions."""

from dataclasses import replace

from workbench.catalog import (
    Catalog,
    ParameterKind,
    SupportStatus,
)
from workbench.registry.builtins import create_builtin_registry
from workbench.registry.definitions import (
    ComponentDefinition,
    ComponentSupport,
    ParameterDefinition,
    ParameterType,
)
from workbench.registry.registry import ComponentRegistry

_PARAMETER_TYPES = {
    ParameterKind.BOOLEAN: ParameterType.BOOLEAN,
    ParameterKind.INTEGER: ParameterType.INTEGER,
    ParameterKind.NUMBER: ParameterType.NUMBER,
    ParameterKind.STRING: ParameterType.STRING,
    ParameterKind.ENUM: ParameterType.ENUM,
}


def _parameter_definition(parameter) -> ParameterDefinition | None:
    value_type = _PARAMETER_TYPES.get(parameter.kind)
    if value_type is None:
        return None
    # Source annotations and Enum aliases may repeat the same serialized
    # choice. Registry schemas require a deterministic unique choice list.
    choices = tuple(dict.fromkeys(parameter.choices))
    if value_type is ParameterType.ENUM and not choices:
        return None
    has_default = parameter.has_default and parameter.default_is_literal
    default = parameter.default if has_default else None
    if (
        isinstance(default, (dict, list, tuple))
        or (default is None and not parameter.nullable)
        or (
            value_type in (ParameterType.STRING, ParameterType.ENUM)
            and default is not None
            and not isinstance(default, str)
        )
        or (
            value_type is ParameterType.BOOLEAN
            and not isinstance(default, bool)
        )
        or (
            value_type is ParameterType.INTEGER
            and (isinstance(default, bool) or not isinstance(default, int))
        )
        or (
            value_type is ParameterType.NUMBER
            and (
                isinstance(default, bool)
                or not isinstance(default, (int, float))
            )
        )
    ):
        has_default = False
        default = None
    return ParameterDefinition(
        parameter.id,
        value_type,
        default,
        has_default=has_default,
        required=parameter.required,
        choices=choices,
        nullable=parameter.nullable,
    )


def create_registry_from_catalog(catalog: Catalog | None) -> ComponentRegistry:
    """Overlay broad discovered metadata on the conservative runnable set."""

    builtins = create_builtin_registry()
    if catalog is None:
        return builtins
    entries_by_id = {entry.type_id: entry for entry in catalog.entries}
    registry = ComponentRegistry()
    for definition in builtins.definitions():
        effective = definition
        if (
            definition.support is ComponentSupport.SUPPORTED
            and definition.type_id.startswith("gem5.stdlib/")
        ):
            entry = entries_by_id.get(definition.type_id)
            reason = None
            if entry is None:
                reason = "Type is not present in the selected gem5 sources"
            elif entry.is_abstract:
                reason = "Type is abstract in the selected gem5 sources"
            elif (
                catalog.binary_fingerprint is not None
                and entry.support_status is not SupportStatus.RUNTIME_CONFIRMED
            ):
                reason = (
                    entry.diagnostics[0].message
                    if entry.diagnostics
                    else "Type is unavailable in the selected gem5 binary"
                )
            if reason is not None:
                effective = replace(
                    definition,
                    support=ComponentSupport.UNAVAILABLE,
                    support_reason=reason,
                )
        registry.register(effective)
    for entry in catalog.entries:
        if registry.get(entry.type_id) is not None:
            continue
        support = (
            ComponentSupport.UNAVAILABLE
            if entry.support_status is SupportStatus.UNAVAILABLE
            else ComponentSupport.EXPERIMENTAL
        )
        if entry.is_abstract:
            reason = "Abstract Standard Library type"
        elif entry.support_status is SupportStatus.UNAVAILABLE:
            reason = "Unavailable in the selected gem5 build"
        else:
            reason = "Discovered automatically; no safe execution adapter yet"
        if entry.diagnostics:
            reason = entry.diagnostics[0].message
        parameters = {}
        for parameter in entry.parameters:
            converted = _parameter_definition(parameter)
            if converted is not None:
                parameters[converted.id] = converted
        registry.register(
            ComponentDefinition(
                entry.type_id,
                entry.display_name,
                entry.category,
                entry.description,
                parameters=parameters,
                support=support,
                support_reason=reason,
                source_reference=(
                    f"{entry.symbol.module}:{entry.symbol.qualified_name}"
                ),
            )
        )
    return registry
