"""Safe, import-free discovery of gem5 Standard Library symbols."""

from __future__ import annotations

import ast
import hashlib
import re
from dataclasses import dataclass
from pathlib import Path

from workbench.catalog.models import (
    Catalog,
    CatalogDiagnostic,
    CatalogEntry,
    DiagnosticSeverity,
    ParameterKind,
    ParameterSpec,
    SymbolKind,
    SymbolReference,
)

TYPE_ID_PREFIX = "gem5.stdlib/"
MAX_SOURCE_FILE_BYTES = 8 * 1024 * 1024
MAX_SOURCE_TREE_BYTES = 128 * 1024 * 1024


class CatalogDiscoveryError(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class _ParsedFile:
    path: Path
    relative_path: str
    module: str
    is_package: bool
    tree: ast.Module
    content: bytes


@dataclass(frozen=True, slots=True)
class _EnumInfo:
    symbol: str
    choices: tuple[str, ...]
    members: tuple[tuple[str, str], ...]


def stable_type_id(module: str, qualified_name: str) -> str:
    """Return the stable ID persisted by projects for a Python symbol."""

    if not module or not qualified_name:
        raise ValueError("module and qualified_name must be non-empty")
    return f"{TYPE_ID_PREFIX}{module}:{qualified_name}"


def discover_source_catalog(gem5_root: str | Path) -> Catalog:
    """Discover component classes and factories without importing gem5.

    ``gem5_root`` may be a gem5 checkout, its ``src/python`` directory, or the
    ``src/python/gem5`` package directory.
    """

    source_root, python_root, gem5_package = _resolve_roots(Path(gem5_root))
    diagnostics: list[CatalogDiagnostic] = []
    parsed: list[_ParsedFile] = []
    # Runtime-probe caches must be invalidated by any change in gem5's Python
    # package, including transitive helpers outside components/prebuilt.
    source_contents = _read_source_snapshot(gem5_package)

    for package_name in ("components", "prebuilt"):
        package_path = gem5_package / package_name
        if not package_path.is_dir():
            diagnostics.append(
                CatalogDiagnostic(
                    "catalog.package_missing",
                    f"gem5.{package_name} was not found",
                    DiagnosticSeverity.WARNING,
                    str(package_path),
                )
            )
            continue
        package_files = (
            path for path in source_contents if package_path in path.parents
        )
        for path in sorted(package_files):
            result = _parse_file(
                path,
                python_root,
                diagnostics,
                content=source_contents[path],
            )
            if result is not None:
                parsed.append(result)

    analysis_failures: set[Path] = set()
    support_files = _parse_imported_enum_files(
        parsed,
        python_root,
        diagnostics,
        source_contents,
        analysis_failures,
    )
    source_fingerprint = _fingerprint_sources(source_contents, python_root)
    enum_index = _collect_enums(
        (*parsed, *support_files), diagnostics, analysis_failures
    )

    entries: list[CatalogEntry] = []
    for item in parsed:
        if item.path in analysis_failures:
            continue
        file_entries: list[CatalogEntry] = []
        try:
            imports = _import_aliases(item)
            for node in item.tree.body:
                if isinstance(node, ast.ClassDef):
                    if node.name.startswith("_") or _is_enum(node):
                        continue
                    file_entries.append(
                        _class_entry(
                            item,
                            node,
                            imports,
                            enum_index,
                            source_fingerprint,
                        )
                    )
                elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    if node.name.startswith("_") or not _is_factory_function(
                        node
                    ):
                        continue
                    file_entries.append(
                        _function_entry(
                            item,
                            node,
                            imports,
                            enum_index,
                            source_fingerprint,
                        )
                    )
        except (RecursionError, MemoryError) as error:
            _record_analysis_failure(item, error, diagnostics)
            analysis_failures.add(item.path)
            continue
        entries.extend(file_entries)

    unique_entries: dict[str, CatalogEntry] = {}
    for entry in entries:
        previous = unique_entries.get(entry.type_id)
        if previous is not None:
            diagnostics.append(
                CatalogDiagnostic(
                    "catalog.duplicate_type_id",
                    "A later definition replaces an earlier symbol with the "
                    f"same stable type ID (line {previous.symbol.line})",
                    DiagnosticSeverity.WARNING,
                    entry.symbol.source_path,
                    entry.symbol.line,
                    entry.type_id,
                )
            )
        # Python module execution also leaves the final same-named definition
        # bound, so keeping the last declaration matches runtime lookup.
        unique_entries[entry.type_id] = entry
    entries = sorted(unique_entries.values(), key=lambda entry: entry.type_id)
    return Catalog(
        source_root=str(source_root),
        entries=tuple(entries),
        source_fingerprint=source_fingerprint,
        diagnostics=tuple(diagnostics),
    )


def fingerprint_source_tree(gem5_root: str | Path) -> str:
    """Return the catalog identity of a gem5 Python source tree.

    This intentionally performs no parsing or imports. It is suitable for
    freshness checks after a potentially long probe and before a simulation.
    """

    _, python_root, gem5_package = _resolve_roots(Path(gem5_root))
    digest = hashlib.sha256()
    total_bytes = 0
    for path in sorted(
        gem5_package.rglob("*.py"), key=lambda item: item.as_posix()
    ):
        content = _read_source_file(path)
        total_bytes += len(content)
        if total_bytes > MAX_SOURCE_TREE_BYTES:
            raise CatalogDiscoveryError(
                "gem5 Python sources exceed the catalog limit of "
                f"{MAX_SOURCE_TREE_BYTES // (1024 * 1024)} MiB"
            )
        relative_path = path.relative_to(python_root).as_posix()
        digest.update(relative_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(content)
        digest.update(b"\0")
    return digest.hexdigest()


def _read_source_file(path: Path) -> bytes:
    with path.open("rb") as stream:
        content = stream.read(MAX_SOURCE_FILE_BYTES + 1)
    if len(content) > MAX_SOURCE_FILE_BYTES:
        raise CatalogDiscoveryError(
            f"gem5 Python source exceeds the {MAX_SOURCE_FILE_BYTES // (1024 * 1024)} "
            f"MiB per-file catalog limit: {path}"
        )
    return content


def _read_source_snapshot(gem5_package: Path) -> dict[Path, bytes]:
    contents: dict[Path, bytes] = {}
    total_bytes = 0
    for path in sorted(
        gem5_package.rglob("*.py"), key=lambda item: item.as_posix()
    ):
        content = _read_source_file(path)
        total_bytes += len(content)
        if total_bytes > MAX_SOURCE_TREE_BYTES:
            raise CatalogDiscoveryError(
                "gem5 Python sources exceed the catalog limit of "
                f"{MAX_SOURCE_TREE_BYTES // (1024 * 1024)} MiB"
            )
        contents[path] = content
    return contents


def _resolve_roots(root: Path) -> tuple[Path, Path, Path]:
    root = root.expanduser().resolve()
    candidates = (
        (root, root / "src" / "python", root / "src" / "python" / "gem5"),
        (root.parent.parent, root, root / "gem5"),
        (root.parent.parent.parent, root.parent, root),
    )
    for source_root, python_root, gem5_package in candidates:
        if (gem5_package / "components").is_dir():
            return source_root, python_root, gem5_package
    raise CatalogDiscoveryError(
        f"{root} does not contain src/python/gem5/components"
    )


def _parse_file(
    path: Path,
    python_root: Path,
    diagnostics: list[CatalogDiagnostic],
    *,
    report_errors: bool = True,
    content: bytes | None = None,
) -> _ParsedFile | None:
    if content is None:
        content = path.read_bytes()
    relative = path.relative_to(python_root).as_posix()
    try:
        text = content.decode("utf-8")
        tree = ast.parse(text, filename=relative)
    except (
        SyntaxError,
        UnicodeDecodeError,
        RecursionError,
        MemoryError,
    ) as error:
        if report_errors:
            detail = str(error) or type(error).__name__
            diagnostics.append(
                CatalogDiagnostic(
                    "catalog.source_parse_failed",
                    detail,
                    DiagnosticSeverity.ERROR,
                    relative,
                    getattr(error, "lineno", None),
                )
            )
        return None
    module_parts = list(Path(relative).with_suffix("").parts)
    is_package = module_parts[-1] == "__init__"
    if is_package:
        module_parts.pop()
    return _ParsedFile(
        path,
        relative,
        ".".join(module_parts),
        is_package,
        tree,
        content,
    )


def _parse_imported_enum_files(
    files: list[_ParsedFile],
    python_root: Path,
    diagnostics: list[CatalogDiagnostic],
    source_contents: dict[Path, bytes],
    analysis_failures: set[Path],
) -> tuple[_ParsedFile, ...]:
    imported_modules: set[str] = {"gem5.isas", "gem5.coherence_protocol"}
    for item in files:
        try:
            for node in ast.walk(item.tree):
                if isinstance(node, ast.ImportFrom):
                    module = _resolve_import_module(
                        item, node.module, node.level
                    )
                    if module.startswith("gem5."):
                        imported_modules.add(module)
        except (RecursionError, MemoryError) as error:
            _record_analysis_failure(item, error, diagnostics)
            analysis_failures.add(item.path)

    results: list[_ParsedFile] = []
    parsed_paths = {item.path for item in files}
    for module in sorted(imported_modules):
        relative = Path(*module.split("."))
        possibilities = (
            python_root / relative.with_suffix(".py"),
            python_root / relative / "__init__.py",
        )
        path = next(
            (item for item in possibilities if item in source_contents), None
        )
        if path is None or path in parsed_paths:
            continue
        content = source_contents.get(path)
        if content is None:
            content = _read_source_file(path)
            source_contents[path] = content
        parsed = _parse_file(
            path,
            python_root,
            diagnostics,
            report_errors=False,
            content=content,
        )
        if parsed is not None:
            results.append(parsed)
            parsed_paths.add(path)
    return tuple(results)


def _fingerprint_sources(
    source_contents: dict[Path, bytes], python_root: Path
) -> str:
    digest = hashlib.sha256()
    for path in sorted(source_contents, key=lambda item: item.as_posix()):
        relative_path = path.relative_to(python_root).as_posix()
        digest.update(relative_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(source_contents[path])
        digest.update(b"\0")
    return digest.hexdigest()


def _record_analysis_failure(
    item: _ParsedFile,
    error: RecursionError | MemoryError,
    diagnostics: list[CatalogDiagnostic],
) -> None:
    detail = str(error) or type(error).__name__
    diagnostics.append(
        CatalogDiagnostic(
            "catalog.source_analysis_failed",
            detail,
            DiagnosticSeverity.ERROR,
            item.relative_path,
        )
    )


def _collect_enums(
    files: tuple[_ParsedFile, ...],
    diagnostics: list[CatalogDiagnostic],
    analysis_failures: set[Path],
) -> dict[str, _EnumInfo]:
    result: dict[str, _EnumInfo] = {}
    for item in files:
        file_result: dict[str, _EnumInfo] = {}
        try:
            for node in item.tree.body:
                if not isinstance(node, ast.ClassDef) or not _is_enum(node):
                    continue
                choices: list[str] = []
                members: list[tuple[str, str]] = []
                for child in node.body:
                    if not isinstance(child, (ast.Assign, ast.AnnAssign)):
                        continue
                    targets = (
                        child.targets
                        if isinstance(child, ast.Assign)
                        else [child.target]
                    )
                    value_node = child.value
                    if value_node is None:
                        continue
                    for target in targets:
                        if not isinstance(
                            target, ast.Name
                        ) or target.id.startswith("_"):
                            continue
                        try:
                            value = ast.literal_eval(value_node)
                        except (ValueError, TypeError, SyntaxError):
                            value = target.id
                        choice = (
                            str(value)
                            if isinstance(value, (str, int, float, bool))
                            else target.id
                        )
                        choices.append(choice)
                        members.append((target.id, choice))
                symbol = f"{item.module}.{node.name}"
                file_result[symbol] = _EnumInfo(
                    symbol, tuple(choices), tuple(members)
                )
        except (RecursionError, MemoryError) as error:
            _record_analysis_failure(item, error, diagnostics)
            analysis_failures.add(item.path)
            continue
        result.update(file_result)
    return result


def _is_enum(node: ast.ClassDef) -> bool:
    return any(
        _annotation_text(base).rsplit(".", 1)[-1] in {"Enum", "StrEnum"}
        for base in node.bases
    )


def _is_factory_function(
    node: ast.FunctionDef | ast.AsyncFunctionDef,
) -> bool:
    name = node.name
    return name[:1].isupper() or name.startswith(
        ("make_", "create_", "build_")
    )


def _class_entry(
    item: _ParsedFile,
    node: ast.ClassDef,
    imports: dict[str, str],
    enum_index: dict[str, _EnumInfo],
    source_fingerprint: str,
) -> CatalogEntry:
    initializer = next(
        (
            child
            for child in node.body
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
            and child.name == "__init__"
        ),
        None,
    )
    parameters = (
        _parameters(
            item, initializer.args, imports, enum_index, skip_first=True
        )
        if initializer is not None
        else ()
    )
    is_abstract = (
        node.name.startswith("Abstract")
        or item.path.stem.startswith("abstract_")
        or any(
            _has_decorator(child, "abstractmethod")
            for child in node.body
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
        )
    )
    return _entry(
        item,
        node.name,
        node.lineno,
        SymbolKind.CLASS,
        parameters,
        ast.get_docstring(node) or "",
        is_abstract,
        source_fingerprint,
    )


def _function_entry(
    item: _ParsedFile,
    node: ast.FunctionDef | ast.AsyncFunctionDef,
    imports: dict[str, str],
    enum_index: dict[str, _EnumInfo],
    source_fingerprint: str,
) -> CatalogEntry:
    return _entry(
        item,
        node.name,
        node.lineno,
        SymbolKind.FACTORY,
        _parameters(item, node.args, imports, enum_index),
        ast.get_docstring(node) or "",
        False,
        source_fingerprint,
    )


def _entry(
    item: _ParsedFile,
    name: str,
    line: int,
    symbol_kind: SymbolKind,
    parameters: tuple[ParameterSpec, ...],
    description: str,
    is_abstract: bool,
    source_fingerprint: str,
) -> CatalogEntry:
    module_parts = item.module.split(".")
    if "prebuilt" in module_parts:
        category = "Prebuilt"
    elif "boards" in module_parts:
        category = "Boards"
    elif "processors" in module_parts:
        category = "Processors"
    elif "cachehierarchies" in module_parts:
        category = "Cache Hierarchies"
    elif "memory" in module_parts:
        category = "Memory"
    else:
        category = "Components"
    return CatalogEntry(
        type_id=stable_type_id(item.module, name),
        display_name=_display_name(name),
        category=category,
        description=_summary(description),
        symbol=SymbolReference(item.module, name, item.relative_path, line),
        symbol_kind=symbol_kind,
        parameters=parameters,
        is_abstract=is_abstract,
        source_fingerprint=source_fingerprint,
    )


def _parameters(
    item: _ParsedFile,
    arguments: ast.arguments,
    imports: dict[str, str],
    enum_index: dict[str, _EnumInfo],
    *,
    skip_first: bool = False,
) -> tuple[ParameterSpec, ...]:
    positional = [*arguments.posonlyargs, *arguments.args]
    if skip_first and positional and positional[0].arg in {"self", "cls"}:
        positional = positional[1:]
    defaults: list[ast.expr | None] = [None] * (
        len(positional) - len(arguments.defaults)
    ) + list(arguments.defaults)

    result: list[ParameterSpec] = []
    positional_only_count = max(
        0, len(arguments.posonlyargs) - (1 if skip_first else 0)
    )
    for index, (argument, default) in enumerate(zip(positional, defaults)):
        result.append(
            _parameter(
                item,
                argument,
                default,
                imports,
                enum_index,
                positional_only=index < positional_only_count,
            )
        )
    if arguments.vararg is not None:
        result.append(
            _parameter(
                item,
                arguments.vararg,
                None,
                imports,
                enum_index,
                variadic=True,
            )
        )
    for argument, default in zip(arguments.kwonlyargs, arguments.kw_defaults):
        result.append(
            _parameter(
                item,
                argument,
                default,
                imports,
                enum_index,
                keyword_only=True,
            )
        )
    if arguments.kwarg is not None:
        result.append(
            _parameter(
                item,
                arguments.kwarg,
                None,
                imports,
                enum_index,
                keyword_only=True,
                variadic=True,
            )
        )
    return tuple(result)


def _parameter(
    item: _ParsedFile,
    argument: ast.arg,
    default_node: ast.expr | None,
    imports: dict[str, str],
    enum_index: dict[str, _EnumInfo],
    *,
    positional_only: bool = False,
    keyword_only: bool = False,
    variadic: bool = False,
) -> ParameterSpec:
    annotation = _annotation_text(argument.annotation) or None
    kind = _parameter_kind(annotation)
    choices = _annotation_choices(
        item, argument.annotation, imports, enum_index
    )
    enum_info = _annotation_enum_info(
        item, argument.annotation, imports, enum_index
    )
    if choices:
        kind = ParameterKind.ENUM
    has_default = default_node is not None
    default_expression = (
        _annotation_text(default_node) if default_node is not None else None
    )
    default: object = None
    default_is_literal = False
    if default_node is not None:
        try:
            default = ast.literal_eval(default_node)
            default_is_literal = _is_catalog_literal(default)
        except (ValueError, TypeError, SyntaxError):
            if (
                isinstance(default_node, ast.Attribute)
                and enum_info is not None
            ):
                enum_members = dict(enum_info.members)
                if default_node.attr in enum_members:
                    default = enum_members[default_node.attr]
                    default_is_literal = True
    return ParameterSpec(
        id=argument.arg,
        kind=kind,
        annotation=annotation,
        required=not has_default and not variadic,
        has_default=has_default,
        default=default if default_is_literal else None,
        default_expression=default_expression,
        default_is_literal=default_is_literal,
        nullable=_is_nullable(annotation)
        or (default_is_literal and default is None),
        choices=choices,
        positional_only=positional_only,
        keyword_only=keyword_only,
        variadic=variadic,
    )


def _annotation_choices(
    item: _ParsedFile,
    annotation: ast.expr | None,
    imports: dict[str, str],
    enum_index: dict[str, _EnumInfo],
) -> tuple[str, ...]:
    if annotation is None:
        return ()
    if (
        isinstance(annotation, ast.Subscript)
        and _annotation_text(annotation.value).rsplit(".", 1)[-1] == "Literal"
    ):
        nodes = (
            annotation.slice.elts
            if isinstance(annotation.slice, ast.Tuple)
            else [annotation.slice]
        )
        values: list[str] = []
        for node in nodes:
            try:
                value = ast.literal_eval(node)
            except (ValueError, TypeError, SyntaxError):
                continue
            if isinstance(value, (str, int, float, bool)):
                values.append(str(value))
        return tuple(values)

    enum_info = _annotation_enum_info(item, annotation, imports, enum_index)
    return enum_info.choices if enum_info is not None else ()


def _annotation_enum_info(
    item: _ParsedFile,
    annotation: ast.expr | None,
    imports: dict[str, str],
    enum_index: dict[str, _EnumInfo],
) -> _EnumInfo | None:
    if annotation is None:
        return None
    if isinstance(annotation, ast.BinOp) and isinstance(
        annotation.op, ast.BitOr
    ):
        return _annotation_enum_info(
            item, annotation.left, imports, enum_index
        ) or _annotation_enum_info(item, annotation.right, imports, enum_index)
    root = annotation
    if isinstance(annotation, ast.Subscript):
        root = annotation.slice
        if isinstance(root, ast.Tuple):
            for child in root.elts:
                enum_info = _annotation_enum_info(
                    item, child, imports, enum_index
                )
                if enum_info is not None:
                    return enum_info
            return None
    symbol = _resolve_annotation_symbol(item, root, imports)
    return enum_index.get(symbol)


def _resolve_annotation_symbol(
    item: _ParsedFile, annotation: ast.expr, imports: dict[str, str]
) -> str:
    text = _annotation_text(annotation)
    first, separator, remainder = text.partition(".")
    if first in imports:
        return imports[first] + (f".{remainder}" if separator else "")
    return f"{item.module}.{text}"


def _import_aliases(item: _ParsedFile) -> dict[str, str]:
    aliases: dict[str, str] = {}
    for node in item.tree.body:
        if isinstance(node, ast.Import):
            for name in node.names:
                aliases[name.asname or name.name.split(".")[0]] = name.name
        elif isinstance(node, ast.ImportFrom):
            module = _resolve_import_module(item, node.module, node.level)
            for name in node.names:
                if name.name != "*":
                    aliases[name.asname or name.name] = f"{module}.{name.name}"
    return aliases


def _resolve_import_module(
    item: _ParsedFile, module: str | None, level: int
) -> str:
    if level == 0:
        return module or ""
    package = item.module.split(".")
    if not item.is_package:
        package.pop()
    remove = max(0, level - 1)
    if remove:
        package = package[:-remove]
    if module:
        package.extend(module.split("."))
    return ".".join(package)


def _parameter_kind(annotation: str | None) -> ParameterKind:
    if annotation is None:
        return ParameterKind.UNKNOWN
    normalized = annotation.replace("typing.", "")
    identifiers = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", normalized))
    primitive_types = identifiers.intersection({"bool", "int", "float", "str"})
    if primitive_types == {"bool"}:
        return ParameterKind.BOOLEAN
    if primitive_types == {"int"}:
        return ParameterKind.INTEGER
    if primitive_types == {"float"}:
        return ParameterKind.NUMBER
    if primitive_types == {"str"} or (
        not primitive_types and "Path" in identifiers
    ):
        return ParameterKind.STRING
    return ParameterKind.UNKNOWN


def _is_nullable(annotation: str | None) -> bool:
    if annotation is None:
        return False
    compact = annotation.replace(" ", "")
    identifiers = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", compact))
    contains_none = bool({"None", "NoneType"}.intersection(identifiers))
    return (
        "Optional" in identifiers
        or compact in {"None", "NoneType", "types.NoneType"}
        or (contains_none and ("|" in compact or "Union" in identifiers))
    )


def _is_catalog_literal(value: object) -> bool:
    if value is None or isinstance(value, (bool, int, str)):
        return True
    if isinstance(value, float):
        return value not in {float("inf"), float("-inf")} and value == value
    if isinstance(value, (list, tuple)):
        return all(_is_catalog_literal(item) for item in value)
    if isinstance(value, dict):
        return all(
            isinstance(key, str) and _is_catalog_literal(item)
            for key, item in value.items()
        )
    return False


def _annotation_text(node: ast.AST | None) -> str:
    return ast.unparse(node) if node is not None else ""


def _has_decorator(
    node: ast.FunctionDef | ast.AsyncFunctionDef, name: str
) -> bool:
    return any(
        _annotation_text(decorator).rsplit(".", 1)[-1] == name
        for decorator in node.decorator_list
    )


def _display_name(name: str) -> str:
    words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name)
    return words.replace("_", " ").strip()


def _summary(docstring: str) -> str:
    return " ".join(docstring.strip().split("\n\n", 1)[0].split())
