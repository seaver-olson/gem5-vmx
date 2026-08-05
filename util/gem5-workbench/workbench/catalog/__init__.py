"""Automatic catalog discovery for a locally installed gem5 checkout."""

from workbench.catalog.cache import (
    CatalogCache,
    catalog_cache_key,
    catalog_from_data,
    catalog_to_data,
)
from workbench.catalog.models import (
    Catalog,
    CatalogDiagnostic,
    CatalogEntry,
    CatalogInstallationIdentity,
    DiagnosticSeverity,
    ParameterKind,
    ParameterSpec,
    RuntimeParameter,
    RuntimeProbeResult,
    RuntimeSymbol,
    SupportStatus,
    SymbolKind,
    SymbolReference,
)
from workbench.catalog.probe import (
    RuntimeProbeError,
    fingerprint_file,
    merge_runtime_probe,
    run_runtime_probe,
)
from workbench.catalog.scanner import (
    CatalogDiscoveryError,
    discover_source_catalog,
    fingerprint_source_tree,
    stable_type_id,
)

__all__ = [
    "Catalog",
    "CatalogCache",
    "CatalogDiagnostic",
    "CatalogDiscoveryError",
    "CatalogEntry",
    "CatalogInstallationIdentity",
    "DiagnosticSeverity",
    "ParameterKind",
    "ParameterSpec",
    "RuntimeParameter",
    "RuntimeProbeError",
    "RuntimeProbeResult",
    "RuntimeSymbol",
    "SupportStatus",
    "SymbolKind",
    "SymbolReference",
    "catalog_cache_key",
    "catalog_from_data",
    "catalog_to_data",
    "discover_source_catalog",
    "fingerprint_file",
    "fingerprint_source_tree",
    "merge_runtime_probe",
    "run_runtime_probe",
    "stable_type_id",
]
