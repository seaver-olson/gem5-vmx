"""Workbench overlays for executable gem5 Standard Library components."""

from workbench.registry.definitions import (
    ComponentDefinition,
    ComponentSupport,
    ParameterDefinition,
    ParameterType,
    PortDefinition,
    PortDirection,
)
from workbench.registry.registry import ComponentRegistry

SIMPLE_BOARD_TYPE_ID = (
    "gem5.stdlib/gem5.components.boards.simple_board:SimpleBoard"
)
SIMPLE_PROCESSOR_TYPE_ID = (
    "gem5.stdlib/gem5.components.processors.simple_processor:SimpleProcessor"
)
DDR3_MEMORY_TYPE_ID = (
    "gem5.stdlib/gem5.components.memory.single_channel:"
    "SingleChannelDDR3_1600"
)
NO_CACHE_TYPE_ID = (
    "gem5.stdlib/gem5.components.cachehierarchies.classic.no_cache:NoCache"
)
SE_WORKLOAD_TYPE_ID = "workbench.adapter/se_binary_workload"

PROCESSOR_INTERFACE = "gem5.stdlib.processor"
MEMORY_INTERFACE = "gem5.stdlib.memory"
CACHE_INTERFACE = "gem5.stdlib.cache_hierarchy"
WORKLOAD_INTERFACE = "gem5.stdlib.workload"


def executable_definitions() -> tuple[ComponentDefinition, ...]:
    """Return the supported x86 vertical-slice overlays."""

    return (
        ComponentDefinition(
            SIMPLE_BOARD_TYPE_ID,
            "Simple Board",
            "Boards",
            "SE-mode board composed from processor, memory, and classic cache",
            ports={
                "processor": PortDefinition(
                    "processor",
                    PortDirection.INPUT,
                    PROCESSOR_INTERFACE,
                    required=True,
                ),
                "memory": PortDefinition(
                    "memory",
                    PortDirection.INPUT,
                    MEMORY_INTERFACE,
                    required=True,
                ),
                "cache_hierarchy": PortDefinition(
                    "cache_hierarchy",
                    PortDirection.INPUT,
                    CACHE_INTERFACE,
                    required=True,
                ),
                "workload": PortDefinition(
                    "workload",
                    PortDirection.INPUT,
                    WORKLOAD_INTERFACE,
                    required=True,
                ),
            },
            parameters={
                "clk_freq": ParameterDefinition(
                    "clk_freq",
                    ParameterType.STRING,
                    "3GHz",
                    has_default=True,
                    required=True,
                )
            },
            source_reference=(
                "gem5.components.boards.simple_board:SimpleBoard"
            ),
        ),
        ComponentDefinition(
            SIMPLE_PROCESSOR_TYPE_ID,
            "Simple Processor",
            "Processors",
            "Homogeneous x86 CPU cores",
            ports={
                "processor": PortDefinition(
                    "processor", PortDirection.OUTPUT, PROCESSOR_INTERFACE
                )
            },
            parameters={
                "cpu_type": ParameterDefinition(
                    "cpu_type",
                    ParameterType.ENUM,
                    "atomic",
                    has_default=True,
                    required=True,
                    choices=("atomic", "timing"),
                ),
                "num_cores": ParameterDefinition(
                    "num_cores",
                    ParameterType.INTEGER,
                    1,
                    has_default=True,
                    required=True,
                    minimum=1,
                    maximum=1024,
                ),
                "isa": ParameterDefinition(
                    "isa",
                    ParameterType.ENUM,
                    "x86",
                    has_default=True,
                    required=True,
                    choices=("x86",),
                    read_only=True,
                ),
            },
            source_reference=(
                "gem5.components.processors.simple_processor:SimpleProcessor"
            ),
        ),
        ComponentDefinition(
            DDR3_MEMORY_TYPE_ID,
            "Single-channel DDR3-1600",
            "Memory",
            "One 64-bit DDR3-1600 channel",
            ports={
                "memory": PortDefinition(
                    "memory", PortDirection.OUTPUT, MEMORY_INTERFACE
                )
            },
            parameters={
                "size": ParameterDefinition(
                    "size",
                    ParameterType.STRING,
                    "32MiB",
                    has_default=True,
                    required=True,
                )
            },
            source_reference=(
                "gem5.components.memory.single_channel:"
                "SingleChannelDDR3_1600"
            ),
        ),
        ComponentDefinition(
            NO_CACHE_TYPE_ID,
            "No Cache",
            "Cache hierarchies",
            "Connect processor cores directly to the memory bus",
            ports={
                "cache_hierarchy": PortDefinition(
                    "cache_hierarchy",
                    PortDirection.OUTPUT,
                    CACHE_INTERFACE,
                )
            },
            source_reference=(
                "gem5.components.cachehierarchies.classic.no_cache:NoCache"
            ),
        ),
        ComponentDefinition(
            SE_WORKLOAD_TYPE_ID,
            "SE Binary Workload",
            "Workloads",
            "A local executable or gem5 resource attached to an SE board",
            ports={
                "workload": PortDefinition(
                    "workload", PortDirection.OUTPUT, WORKLOAD_INTERFACE
                )
            },
            parameters={
                "source_kind": ParameterDefinition(
                    "source_kind",
                    ParameterType.ENUM,
                    "local",
                    has_default=True,
                    required=True,
                    choices=("local", "resource"),
                ),
                "local_path": ParameterDefinition(
                    "local_path",
                    ParameterType.PATH,
                    "tests/test-progs/hello/bin/x86/linux/hello",
                    has_default=True,
                    editor_hint="file",
                ),
                "path_base": ParameterDefinition(
                    "path_base",
                    ParameterType.ENUM,
                    "gem5_root",
                    has_default=True,
                    choices=("gem5_root", "project", "absolute"),
                ),
                "resource_id": ParameterDefinition(
                    "resource_id",
                    ParameterType.STRING,
                    "x86-hello64-static",
                    has_default=True,
                ),
                "resource_version": ParameterDefinition(
                    "resource_version",
                    ParameterType.STRING,
                    "1.0.0",
                    has_default=True,
                    nullable=True,
                ),
            },
            source_reference="SEBinaryWorkload.set_se_binary_workload",
        ),
    )


def _deprecated_definitions() -> tuple[ComponentDefinition, ...]:
    return tuple(
        ComponentDefinition(
            type_id,
            display_name,
            "Legacy",
            "Milestone-one placeholder; replace it with a generated component",
            support=ComponentSupport.DEPRECATED,
            support_reason="Placeholder components cannot be translated to gem5",
        )
        for type_id, display_name in (
            ("board", "Board (legacy)"),
            ("processor", "Processor (legacy)"),
            ("cache", "Cache (legacy)"),
            ("memory", "Memory (legacy)"),
            ("workload", "Workload (legacy)"),
        )
    )


def create_builtin_registry() -> ComponentRegistry:
    registry = ComponentRegistry()
    for definition in executable_definitions() + _deprecated_definitions():
        registry.register(definition)
    return registry
