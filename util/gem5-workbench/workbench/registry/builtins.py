"""Initial component definitions shown in the palette."""

from workbench.registry.definitions import ComponentDefinition
from workbench.registry.registry import ComponentRegistry


def create_builtin_registry() -> ComponentRegistry:
    registry = ComponentRegistry()
    for definition in (
        ComponentDefinition("board", "Board", "System", "System platform"),
        ComponentDefinition("processor", "Processor", "Compute", "CPU cores"),
        ComponentDefinition(
            "cache", "Cache hierarchy", "Memory", "Memory-side caches"
        ),
        ComponentDefinition("memory", "Memory", "Memory", "Physical memory"),
        ComponentDefinition(
            "workload", "Workload", "Software", "Program or resource"
        ),
    ):
        registry.register(definition)
    return registry
