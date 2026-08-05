"""Versioned on-disk cache for merged catalog results."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from dataclasses import replace
from pathlib import Path

from workbench.catalog.models import (
    Catalog,
    CatalogDiagnostic,
    CatalogEntry,
    DiagnosticSeverity,
    ParameterKind,
    ParameterSpec,
    SupportStatus,
    SymbolKind,
    SymbolReference,
    thaw_catalog_value,
)

CACHE_SCHEMA_VERSION = 1
MAX_CATALOG_CACHE_BYTES = 64 * 1024 * 1024


def _unique_json_object(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON object key: {key}")
        value[key] = item
    return value


def _reject_json_constant(value: str) -> object:
    raise ValueError(f"invalid JSON numeric constant: {value}")


def catalog_cache_key(
    source_fingerprint: str, binary_fingerprint: str | None
) -> str:
    if not isinstance(source_fingerprint, str) or not source_fingerprint:
        raise ValueError("source_fingerprint must be a non-empty string")
    if binary_fingerprint is not None and (
        not isinstance(binary_fingerprint, str) or not binary_fingerprint
    ):
        raise ValueError(
            "binary_fingerprint must be a non-empty string or null"
        )
    try:
        source_bytes = source_fingerprint.encode("ascii")
        binary_bytes = (binary_fingerprint or "source-only").encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("catalog fingerprints must contain ASCII") from error
    digest = hashlib.sha256()
    digest.update(source_bytes)
    digest.update(b"\0")
    digest.update(binary_bytes)
    return digest.hexdigest()


def _rebase_path(
    value: str | None, old_root: Path, new_root: Path
) -> str | None:
    if value is None:
        return None
    path = Path(value)
    if not path.is_absolute():
        return value
    try:
        relative = path.relative_to(old_root)
    except ValueError:
        return value
    return str(new_root / relative)


def _rebase_catalog_paths(
    catalog: Catalog, expected_source_root: str
) -> Catalog:
    """Move cached source-owned paths along with an identical checkout."""

    old_root = Path(catalog.source_root)
    new_root = Path(expected_source_root)
    diagnostics = tuple(
        replace(
            diagnostic,
            path=_rebase_path(diagnostic.path, old_root, new_root),
        )
        for diagnostic in catalog.diagnostics
    )
    entries = tuple(
        replace(
            entry,
            symbol=replace(
                entry.symbol,
                source_path=_rebase_path(
                    entry.symbol.source_path, old_root, new_root
                ),
            ),
            diagnostics=tuple(
                replace(
                    diagnostic,
                    path=_rebase_path(diagnostic.path, old_root, new_root),
                )
                for diagnostic in entry.diagnostics
            ),
        )
        for entry in catalog.entries
    )
    return replace(
        catalog,
        source_root=expected_source_root,
        entries=entries,
        diagnostics=diagnostics,
    )


class CatalogCache:
    """Cache catalogs in a caller-selected, untracked local directory."""

    def __init__(self, directory: str | Path) -> None:
        self.directory = Path(directory).expanduser()

    def path_for(
        self, source_fingerprint: str, binary_fingerprint: str | None
    ) -> Path:
        return self.directory / (
            catalog_cache_key(source_fingerprint, binary_fingerprint) + ".json"
        )

    def load(
        self,
        source_fingerprint: str,
        binary_fingerprint: str | None,
        *,
        expected_source_root: str | Path | None = None,
    ) -> Catalog | None:
        """Load a matching catalog and optionally bind it to the current root."""

        path = self.path_for(source_fingerprint, binary_fingerprint)
        try:
            with path.open("rb") as stream:
                payload = stream.read(MAX_CATALOG_CACHE_BYTES + 1)
            if len(payload) > MAX_CATALOG_CACHE_BYTES:
                return None
            data = json.loads(
                payload.decode("utf-8"),
                object_pairs_hook=_unique_json_object,
                parse_constant=_reject_json_constant,
            )
            catalog = catalog_from_data(data)
        except (OSError, ValueError, TypeError, KeyError, RecursionError):
            return None
        if (
            catalog.source_fingerprint != source_fingerprint
            or catalog.binary_fingerprint != binary_fingerprint
        ):
            return None
        if expected_source_root is not None:
            expected = str(Path(expected_source_root).expanduser().resolve())
            if catalog.source_root != expected:
                catalog = _rebase_catalog_paths(catalog, expected)
        return catalog

    def store(self, catalog: Catalog) -> Path:
        self.directory.mkdir(parents=True, exist_ok=True)
        destination = self.path_for(
            catalog.source_fingerprint, catalog.binary_fingerprint
        )
        payload = json.dumps(
            catalog_to_data(catalog),
            indent=2,
            sort_keys=True,
            allow_nan=False,
        )
        if len(payload.encode("utf-8")) > MAX_CATALOG_CACHE_BYTES:
            raise ValueError("catalog exceeds the maximum cache size")
        descriptor, raw_temporary = tempfile.mkstemp(
            prefix=f".{destination.name}.",
            suffix=".tmp",
            dir=self.directory,
            text=True,
        )
        temporary = Path(raw_temporary)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                stream.write(payload)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, destination)
        except BaseException:
            try:
                temporary.unlink()
            except OSError:
                pass
            raise
        return destination


def catalog_to_data(catalog: Catalog) -> dict[str, object]:
    return {
        "schema_version": CACHE_SCHEMA_VERSION,
        "source_root": catalog.source_root,
        "source_fingerprint": catalog.source_fingerprint,
        "binary_fingerprint": catalog.binary_fingerprint,
        "supported_isas": list(catalog.supported_isas),
        "supported_protocols": list(catalog.supported_protocols),
        "diagnostics": [
            _diagnostic_to_data(diagnostic)
            for diagnostic in catalog.diagnostics
        ],
        "entries": [_entry_to_data(entry) for entry in catalog.entries],
    }


def catalog_from_data(data: object) -> Catalog:
    if not isinstance(data, dict):
        raise ValueError("unsupported catalog cache schema")
    schema_version = data.get("schema_version")
    if (
        isinstance(schema_version, bool)
        or not isinstance(schema_version, int)
        or schema_version != CACHE_SCHEMA_VERSION
    ):
        raise ValueError("unsupported catalog cache schema")
    entries = data.get("entries")
    diagnostics = data.get("diagnostics", [])
    if not isinstance(entries, list) or not isinstance(diagnostics, list):
        raise ValueError("catalog entries and diagnostics must be lists")
    binary_fingerprint = data.get("binary_fingerprint")
    if binary_fingerprint is not None and not isinstance(
        binary_fingerprint, str
    ):
        raise ValueError("binary_fingerprint must be a string or null")
    return Catalog(
        source_root=_string(data, "source_root"),
        entries=tuple(_entry_from_data(entry) for entry in entries),
        source_fingerprint=_string(data, "source_fingerprint"),
        diagnostics=tuple(
            _diagnostic_from_data(diagnostic) for diagnostic in diagnostics
        ),
        binary_fingerprint=binary_fingerprint,
        supported_isas=_strings(data.get("supported_isas", [])),
        supported_protocols=_strings(data.get("supported_protocols", [])),
    )


def _entry_to_data(entry: CatalogEntry) -> dict[str, object]:
    return {
        "type_id": entry.type_id,
        "display_name": entry.display_name,
        "category": entry.category,
        "description": entry.description,
        "symbol_kind": entry.symbol_kind.value,
        "support_status": entry.support_status.value,
        "is_abstract": entry.is_abstract,
        "source_fingerprint": entry.source_fingerprint,
        "symbol": {
            "module": entry.symbol.module,
            "qualified_name": entry.symbol.qualified_name,
            "source_path": entry.symbol.source_path,
            "line": entry.symbol.line,
        },
        "parameters": [
            {
                "id": parameter.id,
                "kind": parameter.kind.value,
                "annotation": parameter.annotation,
                "required": parameter.required,
                "has_default": parameter.has_default,
                "default": thaw_catalog_value(parameter.default),
                "default_expression": parameter.default_expression,
                "default_is_literal": parameter.default_is_literal,
                "nullable": parameter.nullable,
                "choices": list(parameter.choices),
                "positional_only": parameter.positional_only,
                "keyword_only": parameter.keyword_only,
                "variadic": parameter.variadic,
            }
            for parameter in entry.parameters
        ],
        "diagnostics": [
            _diagnostic_to_data(diagnostic) for diagnostic in entry.diagnostics
        ],
        "metadata": thaw_catalog_value(entry.metadata),
    }


def _entry_from_data(data: object) -> CatalogEntry:
    if not isinstance(data, dict):
        raise ValueError("catalog entry must be an object")
    symbol = data.get("symbol")
    parameters = data.get("parameters", [])
    diagnostics = data.get("diagnostics", [])
    metadata = data.get("metadata", {})
    if not isinstance(symbol, dict):
        raise ValueError("catalog symbol must be an object")
    if not isinstance(parameters, list) or not isinstance(diagnostics, list):
        raise ValueError("entry parameters and diagnostics must be lists")
    if not isinstance(metadata, dict):
        raise ValueError("entry metadata must be an object")
    return CatalogEntry(
        type_id=_string(data, "type_id"),
        display_name=_string(data, "display_name"),
        category=_string(data, "category"),
        description=_string(data, "description", allow_empty=True),
        symbol=SymbolReference(
            module=_string(symbol, "module"),
            qualified_name=_string(symbol, "qualified_name"),
            source_path=_string(symbol, "source_path"),
            line=_integer(symbol, "line"),
        ),
        symbol_kind=SymbolKind(_string(data, "symbol_kind")),
        parameters=tuple(
            _parameter_from_data(parameter) for parameter in parameters
        ),
        support_status=SupportStatus(_string(data, "support_status")),
        is_abstract=_boolean(data, "is_abstract"),
        source_fingerprint=_string(data, "source_fingerprint"),
        diagnostics=tuple(
            _diagnostic_from_data(diagnostic) for diagnostic in diagnostics
        ),
        metadata=metadata,
    )


def _parameter_from_data(data: object) -> ParameterSpec:
    if not isinstance(data, dict):
        raise ValueError("catalog parameter must be an object")
    return ParameterSpec(
        id=_string(data, "id"),
        kind=ParameterKind(_string(data, "kind")),
        annotation=_optional_string(data, "annotation"),
        required=_boolean(data, "required"),
        has_default=_boolean(data, "has_default"),
        default=data.get("default"),
        default_expression=_optional_string(data, "default_expression"),
        default_is_literal=_boolean(data, "default_is_literal"),
        nullable=_boolean(data, "nullable"),
        choices=_strings(data.get("choices", [])),
        positional_only=_boolean(data, "positional_only"),
        keyword_only=_boolean(data, "keyword_only"),
        variadic=_boolean(data, "variadic"),
    )


def _diagnostic_to_data(
    diagnostic: CatalogDiagnostic,
) -> dict[str, object]:
    return {
        "code": diagnostic.code,
        "message": diagnostic.message,
        "severity": diagnostic.severity.value,
        "path": diagnostic.path,
        "line": diagnostic.line,
        "type_id": diagnostic.type_id,
    }


def _diagnostic_from_data(data: object) -> CatalogDiagnostic:
    if not isinstance(data, dict):
        raise ValueError("catalog diagnostic must be an object")
    line = data.get("line")
    if line is not None and (
        isinstance(line, bool) or not isinstance(line, int)
    ):
        raise ValueError("diagnostic line must be an integer or null")
    return CatalogDiagnostic(
        code=_string(data, "code"),
        message=_string(data, "message", allow_empty=True),
        severity=DiagnosticSeverity(_string(data, "severity")),
        path=_optional_string(data, "path"),
        line=line,
        type_id=_optional_string(data, "type_id"),
    )


def _string(
    data: dict[str, object], key: str, *, allow_empty: bool = False
) -> str:
    value = data.get(key)
    if not isinstance(value, str) or (not value and not allow_empty):
        raise ValueError(f"{key} must be a string")
    return value


def _optional_string(data: dict[str, object], key: str) -> str | None:
    value = data.get(key)
    if value is not None and not isinstance(value, str):
        raise ValueError(f"{key} must be a string or null")
    return value


def _boolean(data: dict[str, object], key: str) -> bool:
    value = data.get(key)
    if not isinstance(value, bool):
        raise ValueError(f"{key} must be a boolean")
    return value


def _integer(data: dict[str, object], key: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be an integer")
    return value


def _strings(value: object) -> tuple[str, ...]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) for item in value
    ):
        raise ValueError("expected a list of strings")
    return tuple(value)
