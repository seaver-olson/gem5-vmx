"""Runtime component definitions and lookup."""

from workbench.registry.builtins import (
    CACHE_INTERFACE,
    DDR3_MEMORY_TYPE_ID,
    MEMORY_INTERFACE,
    NO_CACHE_TYPE_ID,
    PROCESSOR_INTERFACE,
    SE_WORKLOAD_TYPE_ID,
    SIMPLE_BOARD_TYPE_ID,
    SIMPLE_PROCESSOR_TYPE_ID,
    WORKLOAD_INTERFACE,
    create_builtin_registry,
    executable_definitions,
)
from workbench.registry.catalog import create_registry_from_catalog
from workbench.registry.definitions import (
    ComponentDefinition,
    ComponentSupport,
    ParameterDefinition,
    ParameterType,
    PortDefinition,
    PortDirection,
)
from workbench.registry.registry import ComponentRegistry

__all__ = [
    "ComponentDefinition",
    "ComponentRegistry",
    "ComponentSupport",
    "CACHE_INTERFACE",
    "DDR3_MEMORY_TYPE_ID",
    "MEMORY_INTERFACE",
    "NO_CACHE_TYPE_ID",
    "ParameterDefinition",
    "ParameterType",
    "PortDefinition",
    "PortDirection",
    "PROCESSOR_INTERFACE",
    "SE_WORKLOAD_TYPE_ID",
    "SIMPLE_BOARD_TYPE_ID",
    "SIMPLE_PROCESSOR_TYPE_ID",
    "WORKLOAD_INTERFACE",
    "create_builtin_registry",
    "create_registry_from_catalog",
    "executable_definitions",
]
