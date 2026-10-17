#!/usr/bin/env python3
"""Check VM entry semantics, O3 latencies and Minor operation-class support."""

import argparse
import configparser
import re
import subprocess
from pathlib import Path


def check_trace(output, resume_latency, launch_latency):
    config = configparser.ConfigParser()
    config.read(output / "config.ini")
    cycle = int(config["system.clk_domain"]["clock"])
    classes = {"vmresume": "VmxResume", "vmlaunch": "VmxLaunch"}
    latencies = {}
    for name, override in [
        ("vmresume", resume_latency),
        ("vmlaunch", launch_latency),
    ]:
        ops = [
            section
            for section in config.values()
            if section.get("opClass") == classes[name]
        ]
        assert len(ops) == 1
        latencies[name] = int(ops[0]["opLat"])
        assert latencies[name] >= 1
        if override is not None:
            assert latencies[name] == override
        assert ops[0]["pipelined"] == "false"

    issues = {}
    executions = {name: [] for name in classes}
    decoded = {name: [] for name in classes}
    entered = {name: 0 for name in classes}
    executing = None
    required_flags = {
        "IsNonSpeculative",
        "IsSerializing",
        "IsSerializeAfter",
        "IsControl",
        "IsIndirectControl",
        "IsUncondControl",
        "IsSquashAfter",
    }
    with (output / "trace.txt").open() as trace:
        for line in trace:
            issue = re.search(
                r"^(\d+): .*Issuing instruction .*\[sn:(\d+)\]", line
            )
            if issue:
                tick, sequence = map(int, issue.groups())
                issues[sequence] = tick
            execution = re.search(
                r"Execute: Processing PC .*\[sn:(\d+)\]", line
            )
            if execution:
                executing = int(execution[1])
            start = re.search(r"(VMRESUME|VMLAUNCH) start VMCS", line)
            if start:
                tick = int(line.split(":", 1)[0])
                assert executing in issues
                executions[start[1].lower()].append(
                    (executing, tick - issues[executing])
                )
            entry = re.search(
                r"(VMRESUME|VMLAUNCH) entered VMX non-root", line
            )
            if entry:
                entered[entry[1].lower()] += 1
            instruction = re.search(r":\s+(vmresume|vmlaunch)\s+:", line)
            if instruction:
                name = instruction[1]
                assert f": {classes[name]} :" in line
                flags = re.search(r"flags=\(([^)]*)\)", line)[1].split("|")
                assert required_flags <= set(flags)
                decoded[name].append(
                    int(re.search(r"FetchSeq=(\d+)", line)[1])
                )
            if re.search(r":\s+(vmcall|vmxoff)\s+:", line):
                assert ": No_OpClass :" in line
    # Each opcode exercises #UD, VMfailInvalid and VMfailValid, as well as
    # successful entry. Check actual execution timing, not just configuration.
    for name, successes in [("vmresume", 4), ("vmlaunch", 2)]:
        count = 3 + successes
        assert len(executions[name]) == len(decoded[name]) == count
        assert entered[name] == successes and len(set(decoded[name])) == count
        assert [sequence for sequence, _ in executions[name]] == decoded[name]
        assert [delay for _, delay in executions[name]] == [
            latencies[name] * cycle
        ] * count, executions[name]
    return {
        name: [delay // cycle for _, delay in entries]
        for name, entries in executions.items()
    }


def check_minor(output):
    config = configparser.ConfigParser()
    config.read(output / "config.ini")
    supported = set()
    for unit in config["system.cpu.executeFuncUnits"]["funcUnits"].split():
        op_set = config[unit]["opClasses"]
        for op in config[op_set]["opClasses"].split():
            if config[op]["opClass"] in ("VmxResume", "VmxLaunch"):
                assert int(config[unit]["opLat"]) == 1
                supported.add(config[op]["opClass"])
    assert supported == {"VmxResume", "VmxLaunch"}
    # The guest passing is necessary too: a Python-only capability check
    # would miss a decoder still emitting No_OpClass.
    trace = (output / "trace.txt").read_text()
    for name, op_class, count in [
        ("vmresume", "VmxResume", 7),
        ("vmlaunch", "VmxLaunch", 5),
        ("vmcall", "No_OpClass", 7),
        ("vmxoff", "No_OpClass", 2),
    ]:
        entries = re.findall(
            rf"^.*:\s+{name}\s+: {op_class} :.*$", trace, re.MULTILINE
        )
        assert len(entries) == count
        # Only the pre-VMXON attempt faults; ROM delivery must not reexecute it.
        assert sum("FAULTING" in entry for entry in entries) == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=Path("build/X86/gem5.opt")
    )
    parser.add_argument("--outdir", type=Path, default=Path("m5out-vmresume"))
    args = parser.parse_args()
    suite = Path(__file__).resolve().parent
    subprocess.run(
        ["make", "-C", str(suite), "vmresume.elf", "vmx_transition.elf"],
        check=True,
    )
    # Explicit synthetic values keep the independence checks meaningful even
    # after the production defaults are calibrated. Also test both defaults.
    cases = [
        ("atomic", None, None),
        ("timing", None, None),
        ("minor", None, None),
        ("o3", None, None),
        ("o3", 21, 1),
        ("o3", 41, 1),
        ("o3", 1, 21),
        ("o3", 1, 41),
        ("o3", 21, 41),
    ]
    for cpu, resume_latency, launch_latency in cases:
        output = args.outdir / (
            f'{cpu}-resume-{resume_latency or "default"}'
            f'-launch-{launch_latency or "default"}'
        )
        output.mkdir(parents=True, exist_ok=True)
        command = [
            str(args.gem5),
            f"--outdir={output}",
            "--debug-flags=IQ,IEW,VMX,Exec,ExecFlags,ExecFetchSeq",
            "--debug-file=trace.txt",
            str(suite / "config.py"),
            "--guest",
            "vmresume",
            "--cpu",
            cpu,
            "--caches",
        ]
        if resume_latency is not None:
            command += ["--vmresume-latency", str(resume_latency)]
        if launch_latency is not None:
            command += ["--vmlaunch-latency", str(launch_latency)]
        with (output / "console.log").open("w") as log:
            subprocess.run(
                command,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=120,
            )
        if cpu == "o3":
            delays = check_trace(output, resume_latency, launch_latency)
            print(f"{output.name}: PASS; issue-to-execute cycles {delays}")
        else:
            if cpu == "minor":
                check_minor(output)
                print(f"{output.name}: PASS (#UD, VMfail and entry/exit)")
            else:
                print(f"{output.name}: PASS (architectural behavior only)")

    # Keep transition coverage separate from the fixed-count latency traces.
    # The timing case saves a deterministic root-mode checkpoint, continues,
    # then replays the same checks from a fresh simulator process.
    for cpu in ("atomic", "timing", "minor", "o3", "restore"):
        output = args.outdir / f"transition-{cpu}"
        output.mkdir(parents=True, exist_ok=True)
        command = [
            str(args.gem5),
            f"--outdir={output}",
            str(suite / "config.py"),
            "--guest",
            "vmx_transition",
            "--cpu",
            "timing" if cpu == "restore" else cpu,
            "--caches",
        ]
        if cpu == "timing":
            command += ["--vmx-root-checkpoint"]
        elif cpu == "restore":
            checkpoint = args.outdir / "transition-timing" / "checkpoint"
            command += ["--restore", str(checkpoint)]
        with (output / "console.log").open("w") as log:
            subprocess.run(
                command,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=120,
            )
        print(f"{output.name}: PASS (VMX transitions and root state)")


if __name__ == "__main__":
    main()
