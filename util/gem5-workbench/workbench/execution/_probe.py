"""Fixed gem5 config used to inspect a binary without constructing a system."""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    from _m5 import core

    from gem5.runtime import (
        get_supported_isas,
        get_supported_protocols,
    )

    imports = (
        (
            "gem5.stdlib/gem5.components.boards.simple_board:SimpleBoard",
            "gem5.components.boards.simple_board",
            "SimpleBoard",
        ),
        (
            "gem5.stdlib/"
            "gem5.components.processors.simple_processor:SimpleProcessor",
            "gem5.components.processors.simple_processor",
            "SimpleProcessor",
        ),
        (
            "gem5.stdlib/gem5.components.memory.single_channel:"
            "SingleChannelDDR3_1600",
            "gem5.components.memory.single_channel",
            "SingleChannelDDR3_1600",
        ),
        (
            "gem5.stdlib/"
            "gem5.components.cachehierarchies.classic.no_cache:NoCache",
            "gem5.components.cachehierarchies.classic.no_cache",
            "NoCache",
        ),
    )
    importable = []
    for type_id, module_name, symbol_name in imports:
        try:
            module = __import__(module_name, fromlist=[symbol_name])
            getattr(module, symbol_name)
        except (ImportError, AttributeError):
            continue
        importable.append(type_id)
    importable.append("workbench.adapter/se_binary_workload")

    data = {
        "version": core.gem5Version,
        "supported_isas": sorted(isa.value for isa in get_supported_isas()),
        "supported_protocols": sorted(
            protocol.value for protocol in get_supported_protocols()
        ),
        "importable_type_ids": sorted(importable),
    }
    arguments.output.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ in {"__main__", "__m5_main__"}:
    main()
