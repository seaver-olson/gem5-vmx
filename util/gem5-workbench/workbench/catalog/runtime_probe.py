"""Fixed script executed by gem5 to verify catalog imports.

This module is not imported by the Workbench application. Keeping all gem5
imports inside ``main`` ensures the pygame process stays independent of m5.
"""

from __future__ import annotations

import argparse
import importlib
import inspect
import json
import os
from enum import Enum
from pathlib import Path
from typing import get_args


def _enum_choices(annotation: object) -> list[str]:
    candidates = (annotation, *get_args(annotation))
    for candidate in candidates:
        if not inspect.isclass(candidate):
            continue
        try:
            is_enum = issubclass(candidate, Enum)
        except TypeError:
            is_enum = False
        if not is_enum:
            continue
        choices: list[str] = []
        for member in candidate:
            value = member.value
            choices.append(
                str(value)
                if isinstance(value, (str, int, float, bool))
                else member.name
            )
        return choices
    return []


def _annotation_name(annotation: object) -> str | None:
    if annotation is inspect.Parameter.empty:
        return None
    if isinstance(annotation, str):
        return annotation
    return inspect.formatannotation(annotation)


def _default_expression(default: object) -> str | None:
    if default is inspect.Parameter.empty:
        return None
    if isinstance(default, Enum):
        value = default.value
        if isinstance(value, (str, int, float, bool)):
            return repr(value)
        return default.name
    if default is None or isinstance(default, (str, int, float, bool)):
        return repr(default)
    return f"<{type(default).__module__}.{type(default).__qualname__}>"


def _parameters(target: object) -> list[dict[str, object]]:
    signature = inspect.signature(target)
    result: list[dict[str, object]] = []
    for parameter in signature.parameters.values():
        if parameter.name in {"self", "cls"}:
            continue
        result.append(
            {
                "id": parameter.name,
                "annotation": _annotation_name(parameter.annotation),
                "choices": _enum_choices(parameter.annotation),
                "has_default": parameter.default
                is not inspect.Parameter.empty,
                "default_expression": _default_expression(parameter.default),
                "positional_only": parameter.kind
                is inspect.Parameter.POSITIONAL_ONLY,
                "keyword_only": parameter.kind
                is inspect.Parameter.KEYWORD_ONLY,
                "variadic": parameter.kind
                in {
                    inspect.Parameter.VAR_POSITIONAL,
                    inspect.Parameter.VAR_KEYWORD,
                },
            }
        )
    return result


def _resolve_symbol(module_name: str, qualified_name: str) -> object:
    target: object = importlib.import_module(module_name)
    for segment in qualified_name.split("."):
        target = getattr(target, segment)
    return target


def _runtime_capabilities() -> tuple[list[str], list[str]]:
    try:
        runtime = importlib.import_module("gem5.runtime")
        isas = sorted(str(item.value) for item in runtime.get_supported_isas())
        protocols = sorted(
            str(item.value) for item in runtime.get_supported_protocols()
        )
        return isas, protocols
    except Exception:
        return [], []


def _probe_symbol(specification: dict[str, object]) -> dict[str, object]:
    type_id = str(specification["type_id"])
    expected_kind = str(specification["symbol_kind"])
    try:
        target = _resolve_symbol(
            str(specification["module"]),
            str(specification["qualified_name"]),
        )
        actual_kind = (
            "class"
            if inspect.isclass(target)
            else "factory" if callable(target) else None
        )
        if actual_kind is None:
            raise TypeError("resolved object is not a class or callable")
        if actual_kind != expected_kind:
            raise TypeError(
                f"expected {expected_kind}, resolved {actual_kind}"
            )
        try:
            parameters = _parameters(target)
            signature_error = None
        except (TypeError, ValueError) as error:
            parameters = []
            signature_error = f"{type(error).__name__}: {error}"
        return {
            "type_id": type_id,
            "available": True,
            "symbol_kind": actual_kind,
            "parameters": parameters,
            "error": signature_error,
            "is_abstract": (
                inspect.isabstract(target)
                if inspect.isclass(target)
                else False
            ),
        }
    except BaseException as error:
        return {
            "type_id": type_id,
            "available": False,
            "symbol_kind": None,
            "parameters": [],
            "error": f"{type(error).__name__}: {error}",
            "is_abstract": None,
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog-input", required=True)
    parser.add_argument("--catalog-output", required=True)
    arguments = parser.parse_args()

    input_path = Path(arguments.catalog_input)
    output_path = Path(arguments.catalog_output)
    request = json.loads(input_path.read_text(encoding="utf-8"))
    if request.get("schema_version") != 1:
        raise ValueError("unsupported catalog request schema")
    symbols = request.get("symbols")
    if not isinstance(symbols, list):
        raise ValueError("catalog request symbols must be a list")
    isas, protocols = _runtime_capabilities()
    result = {
        "schema_version": 1,
        "source_fingerprint": request["source_fingerprint"],
        "supported_isas": isas,
        "supported_protocols": protocols,
        "symbols": [_probe_symbol(symbol) for symbol in symbols],
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(f".{output_path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(result, sort_keys=True), encoding="utf-8")
    os.replace(temporary, output_path)
    return 0


if __name__ in {"__main__", "__m5_main__"}:
    raise SystemExit(main())
