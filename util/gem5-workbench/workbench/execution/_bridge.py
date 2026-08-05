"""Fixed gem5 configuration bridge for validated Workbench run specs.

This file intentionally imports no Workbench modules. It is launched directly
by a gem5 executable, and all executable symbols are selected from fixed maps.
"""

import argparse
import json
from pathlib import Path

RUN_SPEC_FORMAT = "gem5-workbench-run"
RUN_SPEC_VERSION = 1
SIMPLE_BOARD = "gem5.stdlib/gem5.components.boards.simple_board:SimpleBoard"
SIMPLE_PROCESSOR = (
    "gem5.stdlib/gem5.components.processors.simple_processor:SimpleProcessor"
)
DDR3_MEMORY = (
    "gem5.stdlib/gem5.components.memory.single_channel:"
    "SingleChannelDDR3_1600"
)
NO_CACHE = (
    "gem5.stdlib/" "gem5.components.cachehierarchies.classic.no_cache:NoCache"
)
SE_WORKLOAD = "workbench.adapter/se_binary_workload"
MAX_RUN_SPEC_BYTES = 1024 * 1024


class RunSpecError(ValueError):
    pass


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise RunSpecError(f"duplicate JSON object key: {key}")
        value[key] = item
    return value


def _invalid_constant(value):
    raise RunSpecError(f"invalid JSON numeric constant: {value}")


def _object(value, field):
    if not isinstance(value, dict):
        raise RunSpecError(f"{field} must be an object")
    return value


def _exact_keys(value, expected, field):
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(repr(key) for key in actual - expected)
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unexpected " + ", ".join(extra))
        raise RunSpecError(f"{field} has invalid fields: {'; '.join(details)}")


def _text(value, field):
    if not isinstance(value, str) or not value.strip():
        raise RunSpecError(f"{field} must be a non-empty string")
    return value


def _component(data, role, type_id, parameter_keys):
    component = _object(data, f"components.{role}")
    _exact_keys(component, {"type_id", "parameters"}, f"components.{role}")
    if component["type_id"] != type_id:
        raise RunSpecError(f"components.{role}.type_id is not supported")
    parameters = _object(
        component["parameters"], f"components.{role}.parameters"
    )
    _exact_keys(parameters, parameter_keys, f"components.{role}.parameters")
    return parameters


def validate_run_spec(value):
    data = _object(value, "run spec")
    _exact_keys(
        data,
        {"format", "schema_version", "isa", "components", "component_ids"},
        "run spec",
    )
    if data["format"] != RUN_SPEC_FORMAT:
        raise RunSpecError("unsupported run spec format")
    schema_version = data["schema_version"]
    if (
        isinstance(schema_version, bool)
        or not isinstance(schema_version, int)
        or schema_version != RUN_SPEC_VERSION
    ):
        raise RunSpecError("unsupported run spec schema version")
    if data["isa"] != "x86":
        raise RunSpecError("only the X86 execution adapter is supported")

    components = _object(data["components"], "components")
    roles = {"board", "processor", "memory", "cache_hierarchy", "workload"}
    _exact_keys(components, roles, "components")
    ids = _object(data["component_ids"], "component_ids")
    _exact_keys(ids, roles, "component_ids")
    for role, component_id in ids.items():
        _text(component_id, f"component_ids.{role}")

    board = _component(
        components["board"], "board", SIMPLE_BOARD, {"clk_freq"}
    )
    processor = _component(
        components["processor"],
        "processor",
        SIMPLE_PROCESSOR,
        {"cpu_type", "num_cores", "isa"},
    )
    memory = _component(components["memory"], "memory", DDR3_MEMORY, {"size"})
    cache = _component(
        components["cache_hierarchy"], "cache_hierarchy", NO_CACHE, set()
    )
    workload_component = _object(components["workload"], "components.workload")
    _exact_keys(
        workload_component,
        {"type_id", "parameters"},
        "components.workload",
    )
    if workload_component["type_id"] != SE_WORKLOAD:
        raise RunSpecError("components.workload.type_id is not supported")
    workload = _object(
        workload_component["parameters"], "components.workload.parameters"
    )

    _text(board["clk_freq"], "board.clk_freq")
    _text(memory["size"], "memory.size")
    cpu_type = _text(processor["cpu_type"], "processor.cpu_type")
    if cpu_type not in {"atomic", "timing"}:
        raise RunSpecError("processor.cpu_type is not supported")
    processor_isa = _text(processor["isa"], "processor.isa")
    if processor_isa != "x86":
        raise RunSpecError("processor.isa must be x86")
    cores = processor["num_cores"]
    if (
        isinstance(cores, bool)
        or not isinstance(cores, int)
        or not 1 <= cores <= 1024
    ):
        raise RunSpecError("processor.num_cores must be between 1 and 1024")
    if cache:
        raise RunSpecError("NoCache does not accept parameters")

    kind = workload.get("kind")
    if kind == "local":
        _exact_keys(workload, {"kind", "path", "arguments"}, "workload")
        _text(workload["path"], "workload.path")
    elif kind == "resource":
        allowed = {"kind", "resource_id", "resource_version", "arguments"}
        if "resource_version" not in workload:
            allowed.remove("resource_version")
        _exact_keys(workload, allowed, "workload")
        _text(workload["resource_id"], "workload.resource_id")
        if "resource_version" in workload:
            _text(workload["resource_version"], "workload.resource_version")
    else:
        raise RunSpecError("workload.kind must be local or resource")
    arguments = workload["arguments"]
    if not isinstance(arguments, list) or not all(
        isinstance(argument, str) for argument in arguments
    ):
        raise RunSpecError("workload.arguments must be an array of strings")
    return data


def load_run_spec(path):
    try:
        with Path(path).open("rb") as stream:
            raw_value = stream.read(MAX_RUN_SPEC_BYTES + 1)
        if len(raw_value) > MAX_RUN_SPEC_BYTES:
            raise RunSpecError(f"run spec exceeds {MAX_RUN_SPEC_BYTES} bytes")
        value = json.loads(
            raw_value.decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=_invalid_constant,
        )
    except RunSpecError:
        raise
    except (OSError, UnicodeError, ValueError, RecursionError) as error:
        raise RunSpecError(f"could not load run spec: {error}") from error
    return validate_run_spec(value)


def execute(spec):
    from gem5.components.boards.simple_board import SimpleBoard
    from gem5.components.cachehierarchies.classic.no_cache import NoCache
    from gem5.components.memory.single_channel import SingleChannelDDR3_1600
    from gem5.components.processors.cpu_types import CPUTypes
    from gem5.components.processors.simple_processor import SimpleProcessor
    from gem5.isas import ISA
    from gem5.resources.resource import (
        BinaryResource,
        obtain_resource,
    )
    from gem5.simulate.simulator import Simulator
    from gem5.utils.requires import requires

    components = spec["components"]
    board_parameters = components["board"]["parameters"]
    processor_parameters = components["processor"]["parameters"]
    memory_parameters = components["memory"]["parameters"]
    workload_parameters = components["workload"]["parameters"]

    requires(isa_required=ISA.X86)
    cache_hierarchy = NoCache()
    memory = SingleChannelDDR3_1600(size=memory_parameters["size"])
    cpu_types = {
        "atomic": CPUTypes.ATOMIC,
        "timing": CPUTypes.TIMING,
    }
    processor = SimpleProcessor(
        cpu_type=cpu_types[processor_parameters["cpu_type"]],
        num_cores=processor_parameters["num_cores"],
        isa=ISA.X86,
    )
    board = SimpleBoard(
        clk_freq=board_parameters["clk_freq"],
        processor=processor,
        memory=memory,
        cache_hierarchy=cache_hierarchy,
    )
    if workload_parameters["kind"] == "local":
        binary = BinaryResource(
            local_path=workload_parameters["path"], architecture=ISA.X86
        )
    else:
        binary = obtain_resource(
            workload_parameters["resource_id"],
            resource_version=workload_parameters.get("resource_version"),
        )
    board.set_se_binary_workload(
        binary, arguments=workload_parameters["arguments"]
    )
    Simulator(board=board).run()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", type=Path, required=True)
    arguments = parser.parse_args()
    execute(load_run_spec(arguments.spec))


if __name__ in {"__main__", "__m5_main__"}:
    main()
