"""Validated component definition registry."""

import math

from workbench.model.values import clone_json_value
from workbench.registry.definitions import (
    ComponentDefinition,
    ComponentSupport,
    ParameterDefinition,
    ParameterType,
    PortDefinition,
    PortDirection,
)


def _require_text(value: str, field: str) -> None:
    if not isinstance(value, str) or not value or value != value.strip():
        raise ValueError(
            f"{field} must be a non-empty string without surrounding whitespace"
        )


def _matches_type(value: object, expected: ParameterType) -> bool:
    if expected is ParameterType.BOOLEAN:
        return isinstance(value, bool)
    if expected is ParameterType.INTEGER:
        return isinstance(value, int) and not isinstance(value, bool)
    if expected is ParameterType.NUMBER:
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, str)


def _validate_bound(value: float | None, field: str) -> None:
    if value is not None and (
        isinstance(value, bool) or not isinstance(value, (int, float))
    ):
        raise ValueError(f"{field} must be a number or null")
    if value is not None:
        try:
            finite = math.isfinite(value)
        except OverflowError:
            finite = False
        if not finite:
            raise ValueError(f"{field} must be finite")


class ComponentRegistry:
    def __init__(self) -> None:
        self._definitions: dict[str, ComponentDefinition] = {}

    def register(self, definition: ComponentDefinition) -> None:
        if not isinstance(definition, ComponentDefinition):
            raise ValueError(
                "registry entries must be ComponentDefinition objects"
            )
        _require_text(definition.type_id, "component type id")
        _require_text(definition.display_name, "component display name")
        _require_text(definition.category, "component category")
        if not isinstance(definition.description, str):
            raise ValueError("component description must be a string")
        if not isinstance(definition.support, ComponentSupport):
            raise ValueError("component support must be a ComponentSupport")
        if not isinstance(definition.support_reason, str):
            raise ValueError("component support reason must be a string")
        if definition.source_reference is not None and not isinstance(
            definition.source_reference, str
        ):
            raise ValueError("component source reference must be a string")
        if definition.type_id in self._definitions:
            raise ValueError(f"duplicate component type: {definition.type_id}")

        for key, port in definition.ports.items():
            if not isinstance(port, PortDefinition):
                raise ValueError(f"port {key!r} must be a PortDefinition")
            _require_text(key, "port dictionary key")
            _require_text(port.id, "port id")
            _require_text(port.interface, f"port {port.id} interface")
            if key != port.id:
                raise ValueError(
                    f"port dictionary key {key!r} does not match id {port.id!r}"
                )
            if not isinstance(port.direction, PortDirection):
                raise ValueError(f"port {port.id} has an invalid direction")
            if not isinstance(port.required, bool) or not isinstance(
                port.vector, bool
            ):
                raise ValueError(
                    f"port {port.id} required and vector flags must be boolean"
                )
            maximum = port.maximum_connections
            if maximum is not None and (
                isinstance(maximum, bool)
                or not isinstance(maximum, int)
                or maximum < 1
            ):
                raise ValueError(
                    f"port {port.id} maximum_connections must be positive or null"
                )

        for key, parameter in definition.parameters.items():
            if not isinstance(parameter, ParameterDefinition):
                raise ValueError(
                    f"parameter {key!r} must be a ParameterDefinition"
                )
            _require_text(key, "parameter dictionary key")
            _require_text(parameter.id, "parameter id")
            if key != parameter.id:
                raise ValueError(
                    "parameter dictionary key "
                    f"{key!r} does not match id {parameter.id!r}"
                )
            if not isinstance(parameter.value_type, ParameterType):
                raise ValueError(
                    f"parameter {parameter.id} has an invalid value type"
                )
            if not isinstance(parameter.has_default, bool) or not isinstance(
                parameter.required, bool
            ):
                raise ValueError(
                    f"parameter {parameter.id} flags must be boolean"
                )
            if not isinstance(parameter.nullable, bool) or not isinstance(
                parameter.read_only, bool
            ):
                raise ValueError(
                    f"parameter {parameter.id} editor flags must be boolean"
                )
            if parameter.editor_hint is not None and not isinstance(
                parameter.editor_hint, str
            ):
                raise ValueError(
                    f"parameter {parameter.id} editor hint must be a string"
                )
            _validate_bound(
                parameter.minimum, f"parameter {parameter.id} minimum"
            )
            _validate_bound(
                parameter.maximum, f"parameter {parameter.id} maximum"
            )
            if (
                parameter.minimum is not None
                and parameter.maximum is not None
                and parameter.minimum > parameter.maximum
            ):
                raise ValueError(
                    f"parameter {parameter.id} minimum exceeds maximum"
                )
            if (
                parameter.minimum is not None or parameter.maximum is not None
            ) and parameter.value_type not in {
                ParameterType.INTEGER,
                ParameterType.NUMBER,
            }:
                raise ValueError(
                    f"parameter {parameter.id} bounds require a numeric type"
                )
            if (
                parameter.read_only
                and parameter.required
                and not parameter.has_default
            ):
                raise ValueError(
                    f"read-only required parameter {parameter.id} needs a default"
                )
            if not parameter.has_default and parameter.default is not None:
                raise ValueError(
                    f"parameter {parameter.id} supplies a default while "
                    "has_default is false"
                )
            clone_json_value(
                parameter.default, f"parameter {parameter.id} default"
            )
            default_matches = (
                parameter.default is None and parameter.nullable
            ) or _matches_type(parameter.default, parameter.value_type)
            if parameter.has_default and not default_matches:
                raise ValueError(
                    f"parameter {parameter.id} default does not match "
                    f"{parameter.value_type.value}"
                )
            if (
                parameter.has_default
                and parameter.default is not None
                and parameter.choices
                and parameter.default not in parameter.choices
            ):
                raise ValueError(
                    f"parameter {parameter.id} default is not an allowed choice"
                )
            if (
                parameter.has_default
                and isinstance(parameter.default, (int, float))
                and not isinstance(parameter.default, bool)
                and parameter.minimum is not None
                and parameter.default < parameter.minimum
            ):
                raise ValueError(
                    f"parameter {parameter.id} default is below its minimum"
                )
            if (
                parameter.has_default
                and isinstance(parameter.default, (int, float))
                and not isinstance(parameter.default, bool)
                and parameter.maximum is not None
                and parameter.default > parameter.maximum
            ):
                raise ValueError(
                    f"parameter {parameter.id} default exceeds its maximum"
                )
            for choice in parameter.choices:
                clone_json_value(choice, f"parameter {parameter.id} choice")
                if not _matches_type(choice, parameter.value_type):
                    raise ValueError(
                        f"parameter {parameter.id} choice does not match "
                        f"{parameter.value_type.value}"
                    )
                if (
                    isinstance(choice, (int, float))
                    and not isinstance(choice, bool)
                    and parameter.minimum is not None
                    and choice < parameter.minimum
                ):
                    raise ValueError(
                        f"parameter {parameter.id} choice is below its minimum"
                    )
                if (
                    isinstance(choice, (int, float))
                    and not isinstance(choice, bool)
                    and parameter.maximum is not None
                    and choice > parameter.maximum
                ):
                    raise ValueError(
                        f"parameter {parameter.id} choice exceeds its maximum"
                    )
            if parameter.value_type is ParameterType.ENUM and not (
                parameter.choices
            ):
                raise ValueError(
                    f"enum parameter {parameter.id} requires choices"
                )
            if len({repr(choice) for choice in parameter.choices}) != len(
                parameter.choices
            ):
                raise ValueError(
                    f"parameter {parameter.id} choices must be unique"
                )

        self._definitions[definition.type_id] = definition

    def get(self, type_id: str) -> ComponentDefinition | None:
        return self._definitions.get(type_id)

    def definitions(self) -> tuple[ComponentDefinition, ...]:
        return tuple(self._definitions.values())
