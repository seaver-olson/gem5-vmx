"""JSON-compatible values accepted by the persistent domain model."""

import math
from types import MappingProxyType
from typing import (
    Mapping,
    TypeAlias,
)

JsonValue: TypeAlias = (
    None
    | bool
    | int
    | float
    | str
    | list["JsonValue"]
    | dict[str, "JsonValue"]
)
FrozenJsonValue: TypeAlias = (
    None
    | bool
    | int
    | float
    | str
    | tuple["FrozenJsonValue", ...]
    | Mapping[str, "FrozenJsonValue"]
)

MAX_JSON_NESTING = 100


def _check_nesting(depth: int, field: str) -> None:
    if depth > MAX_JSON_NESTING:
        raise ValueError(
            f"{field} exceeds the maximum JSON nesting depth of "
            f"{MAX_JSON_NESTING}"
        )


def clone_json_value(
    value: JsonValue, field: str = "value", _depth: int = 0
) -> JsonValue:
    """Validate and detach a JSON value used by the mutation API."""

    _check_nesting(_depth, field)
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        try:
            finite = math.isfinite(value)
        except OverflowError:
            finite = False
        if not finite:
            raise ValueError(f"{field} must be a finite JSON number")
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"{field} must be a finite JSON number")
        return value
    if isinstance(value, list):
        return [
            clone_json_value(item, f"{field}[{index}]", _depth + 1)
            for index, item in enumerate(value)
        ]
    if isinstance(value, dict):
        result: dict[str, JsonValue] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError(f"{field} keys must be strings")
            result[key] = clone_json_value(item, f"{field}.{key}", _depth + 1)
        return result
    raise ValueError(f"{field} must be JSON-compatible")


def clone_json_object(
    value: dict[str, JsonValue], field: str = "value"
) -> dict[str, JsonValue]:
    result = clone_json_value(value, field)
    if not isinstance(result, dict):
        raise ValueError(f"{field} must be an object")
    return result


def freeze_json_value(
    value: JsonValue, field: str = "value", _depth: int = 0
) -> FrozenJsonValue:
    _check_nesting(_depth, field)
    if isinstance(value, (list, tuple)):
        return tuple(
            freeze_json_value(item, f"{field}[{index}]", _depth + 1)
            for index, item in enumerate(value)
        )
    if isinstance(value, Mapping):
        result: dict[str, FrozenJsonValue] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError(f"{field} keys must be strings")
            result[key] = freeze_json_value(item, f"{field}.{key}", _depth + 1)
        return MappingProxyType(result)
    return clone_json_value(value, field, _depth)


def freeze_json_object(
    value: Mapping[str, JsonValue], field: str = "value"
) -> Mapping[str, FrozenJsonValue]:
    frozen = freeze_json_value(dict(value), field)
    if not isinstance(frozen, Mapping):
        raise ValueError(f"{field} must be an object")
    return frozen


def thaw_json_value(value: FrozenJsonValue) -> JsonValue:
    if isinstance(value, tuple):
        return [thaw_json_value(item) for item in value]
    if isinstance(value, Mapping):
        return {key: thaw_json_value(item) for key, item in value.items()}
    return value


def thaw_json_object(
    value: Mapping[str, FrozenJsonValue],
) -> dict[str, JsonValue]:
    thawed = thaw_json_value(value)
    if not isinstance(thawed, dict):
        raise ValueError("frozen JSON object did not thaw to an object")
    return thawed
