"""JSON-compatible values accepted by the persistent domain model."""

import math
from typing import TypeAlias


JsonValue: TypeAlias = (
    None
    | bool
    | int
    | float
    | str
    | list["JsonValue"]
    | dict[str, "JsonValue"]
)


def clone_json_value(value: JsonValue, field: str = "value") -> JsonValue:
    """Validate and detach a JSON value used by the mutation API."""

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
            clone_json_value(item, f"{field}[{index}]")
            for index, item in enumerate(value)
        ]
    if isinstance(value, dict):
        result: dict[str, JsonValue] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError(f"{field} keys must be strings")
            result[key] = clone_json_value(item, f"{field}.{key}")
        return result
    raise ValueError(f"{field} must be JSON-compatible")


def clone_json_object(
    value: dict[str, JsonValue], field: str = "value"
) -> dict[str, JsonValue]:
    result = clone_json_value(value, field)
    if not isinstance(result, dict):
        raise ValueError(f"{field} must be an object")
    return result
