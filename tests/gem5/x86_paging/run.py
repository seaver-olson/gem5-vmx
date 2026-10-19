#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run the disk-free paging CPU/memory matrix and retain per-case evidence."""

import argparse
import json
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=Path("build/X86/gem5.opt")
    )
    parser.add_argument(
        "--outdir", type=Path, default=Path("m5out-paging-matrix")
    )
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument(
        "--transport-only",
        action="store_true",
        help="Run expanded transport fixtures separately from baseline",
    )
    parser.add_argument("--vmx-operands-only", action="store_true")
    parser.add_argument(
        "--ept-only",
        action="store_true",
        help="Run EPT fixtures and guest entry/exit tests",
    )
    args = parser.parse_args()
    if sum((args.transport_only, args.vmx_operands_only, args.ept_only)) > 1:
        parser.error("Select only one focused suite")
    suite = Path(__file__).resolve().parent
    subprocess.run(["make", "-C", str(suite)], check=True)
    args.outdir.mkdir(parents=True, exist_ok=True)
    cases = []
    for guest in ("paging", "legacy", "pae"):
        for cpu in ("atomic", "timing", "o3"):
            for caches in (False, True):
                cases.append(
                    (
                        f"{guest}-{cpu}-classic-{int(caches)}",
                        "config.py",
                        ["--cpu", cpu, "--guest", guest]
                        + (["--caches"] if caches else []),
                    )
                )
            if cpu != "atomic":
                cases.append(
                    (
                        f"{guest}-{cpu}-ruby",
                        "ruby.py",
                        ["--cpu", cpu, "--guest", guest],
                    )
                )
    # Minor must deliver instruction-fetch faults without refetching the
    # inaccessible PC while dispatching exception ROM. Reuse the same guest's
    # cold/warm NX, nonpresent/reserved-entry, CR2 and handler-return checks.
    for caches in (False, True):
        cases.append(
            (
                f"paging-minor-classic-{int(caches)}",
                "config.py",
                ["--cpu", "minor", "--guest", "paging"]
                + (["--caches"] if caches else []),
            )
        )
    for hierarchy in ("l1", "private-l2", "shared-l2"):
        for cpu in ("atomic", "timing", "o3"):
            cases.append(
                (
                    f"stdlib-{hierarchy}-{cpu}",
                    "ruby.py",
                    ["--cpu", cpu, "--classic", hierarchy],
                )
            )
    checkpoint = args.outdir / "checkpoint-source" / "checkpoint"
    cases.extend(
        [
            (
                "checkpoint-source",
                "config.py",
                ["--cpu", "timing", "--caches", "--checkpoint-at", "6025000"],
            ),
            (
                "checkpoint-restore",
                "config.py",
                ["--cpu", "timing", "--caches", "--restore", str(checkpoint)],
            ),
        ]
    )
    for cpu in ("atomic", "timing"):
        for caches in (False, True):
            cases.append(
                (
                    f"fixture-{cpu}-{int(caches)}",
                    "config.py",
                    ["--cpu", cpu, "--fixture"]
                    + (["--caches"] if caches else []),
                )
            )
    cases.append(("fixture-ruby", "ruby.py", ["--cpu", "timing", "--fixture"]))
    for hierarchy in ("ruby", "l1", "private-l2", "shared-l2"):
        cases.append(
            (
                f"fixture-peers-{hierarchy}",
                "ruby.py",
                ["--cpu", "timing", "--fixture-peers"]
                + ([] if hierarchy == "ruby" else ["--classic", hierarchy]),
            )
        )
    for source, target in (
        ("atomic", "timing"),
        ("timing", "o3"),
        ("o3", "atomic"),
    ):
        cases.append(
            (
                f"switch-{source}-{target}",
                "config.py",
                [
                    "--cpu",
                    source,
                    "--switch-to",
                    target,
                    "--caches",
                    "--switch-at",
                    "1000000" if source == "atomic" else "6025000",
                ],
            )
        )
    results = []
    pae_checkpoint = args.outdir / "pae-checkpoint" / "checkpoint"
    cases.extend(
        [
            (
                "pae-checkpoint",
                "config.py",
                [
                    "--cpu",
                    "timing",
                    "--caches",
                    "--guest",
                    "pae",
                    "--pae-checkpoint",
                ],
            ),
            (
                "pae-restore",
                "config.py",
                [
                    "--cpu",
                    "timing",
                    "--caches",
                    "--guest",
                    "pae",
                    "--restore",
                    str(pae_checkpoint),
                ],
            ),
        ]
    )
    for source, target in (
        ("atomic", "timing"),
        ("timing", "o3"),
        ("o3", "atomic"),
    ):
        cases.append(
            (
                f"pae-switch-{source}-{target}",
                "config.py",
                [
                    "--cpu",
                    source,
                    "--switch-to",
                    target,
                    "--caches",
                    "--guest",
                    "pae",
                ],
            )
        )
    cases.append(("se-contexts", "se.py", []))
    for cpu in ("atomic", "timing", "o3"):
        cases.append(
            (f"pcid-{cpu}", "config.py", ["--cpu", cpu, "--caches", "--pcid"])
        )
    if args.transport_only:
        cases = [
            (
                f"transport-classic-{int(caches)}",
                "config.py",
                ["--cpu", "timing", "--transport"]
                + (["--caches"] if caches else []),
            )
            for caches in (False, True)
        ]
        for hierarchy in ("ruby", "l1", "private-l2", "shared-l2"):
            cases.append(
                (
                    f"transport-{hierarchy}",
                    "ruby.py",
                    ["--cpu", "timing", "--fixture-peers", "--transport"]
                    + (
                        [] if hierarchy == "ruby" else ["--classic", hierarchy]
                    ),
                )
            )
    if args.vmx_operands_only:
        cases = []
        for cpu in ("atomic", "timing", "o3"):
            for caches in (False, True):
                cases.append(
                    (
                        f"vmx-operands-{cpu}-classic-{int(caches)}",
                        "config.py",
                        ["--cpu", cpu, "--guest", "vmx_operands"]
                        + (["--caches"] if caches else []),
                    )
                )
            if cpu != "atomic":
                cases.append(
                    (
                        f"vmx-operands-{cpu}-ruby",
                        "ruby.py",
                        ["--cpu", cpu, "--guest", "vmx_operands"],
                    )
                )
    if args.ept_only:
        # Each context compares its memory mode against functional lookup.
        # Also exercise guest-MMU access/fault paths and nested lifecycle.
        cases = [
            (
                f"ept-{cpu}-classic-{int(caches)}",
                "config.py",
                ["--cpu", cpu, "--ept-only"]
                + (["--caches"] if caches else []),
            )
            for cpu in ("atomic", "timing")
            for caches in (False, True)
        ]
        cases += [
            (
                f"ept-entry-{cpu}",
                "config.py",
                ["--cpu", cpu, "--guest", "ept_entry", "--caches"],
            )
            for cpu in ("atomic", "timing", "minor", "o3")
        ]
        cases += [
            (
                f"ept-wide-gpa-{cpu}",
                "config.py",
                [
                    "--cpu",
                    cpu,
                    "--guest",
                    "ept_entry",
                    "--caches",
                    "--physical-bits",
                    "52",
                ],
            )
            for cpu in ("atomic", "timing", "minor", "o3")
        ]
        cases += [
            (
                f"vmx-nmi-{cpu}",
                "config.py",
                ["--cpu", cpu, "--guest", "vmx_nmi", "--caches"],
            )
            for cpu in ("atomic", "timing", "minor", "o3")
        ]
        for cpu in ("atomic", "timing", "minor", "o3"):
            cases.append(
                (
                    f"vmx-migrate-{cpu}",
                    "ruby.py",
                    ["--cpu", cpu, "--classic", "l1", "--guest", "vmx_migrate"]
                    + (["--vmx-root-checkpoint"] if cpu == "timing" else []),
                )
            )
        migration_checkpoint = (
            args.outdir / "vmx-migrate-timing" / "checkpoint"
        )
        cases.append(
            (
                "vmx-migrate-restore",
                "ruby.py",
                [
                    "--cpu",
                    "timing",
                    "--classic",
                    "l1",
                    "--guest",
                    "vmx_migrate",
                    "--restore",
                    str(migration_checkpoint),
                ],
            )
        )
        for hierarchy in ("private-l2", "shared-l2", "ruby"):
            cases.append(
                (
                    f"vmx-migrate-{hierarchy}",
                    "ruby.py",
                    ["--cpu", "timing", "--guest", "vmx_migrate"]
                    + (
                        [] if hierarchy == "ruby" else ["--classic", hierarchy]
                    ),
                )
            )
        for width in (32, 40, 48, 52):
            for cpu in ("atomic", "timing", "minor", "o3"):
                cases.append(
                    (
                        f"vmx-address-{width}-{cpu}",
                        "config.py",
                        [
                            "--cpu",
                            cpu,
                            "--guest",
                            "vmx_address",
                            "--caches",
                            "--physical-bits",
                            str(width),
                        ]
                        + (["--vmx-high-memory"] if width == 52 else [])
                        + (
                            ["--vmx-root-checkpoint"]
                            if width == 52 and cpu == "timing"
                            else []
                        ),
                    )
                )
        address_checkpoint = (
            args.outdir / "vmx-address-52-timing" / "checkpoint"
        )
        for width in (52, 40):
            cases.append(
                (
                    (
                        "vmx-address-restore"
                        if width == 52
                        else "vmx-address-narrowed-restore"
                    ),
                    "config.py",
                    [
                        "--cpu",
                        "timing",
                        "--guest",
                        "vmx_address",
                        "--caches",
                        "--physical-bits",
                        str(width),
                        "--vmx-high-memory",
                        "--restore",
                        str(address_checkpoint),
                    ],
                )
            )
        cases += [
            (
                f"vmx-nmi-{cpu}-uncached",
                "config.py",
                ["--cpu", cpu, "--guest", "vmx_nmi"],
            )
            for cpu in ("atomic", "timing")
        ]
        for hierarchy in ("l1", "private-l2", "shared-l2", "ruby"):
            cases.append(
                (
                    f"ept-peers-{hierarchy}",
                    "ruby.py",
                    ["--cpu", "timing", "--ept-only", "--fixture-peers"]
                    + (
                        [] if hierarchy == "ruby" else ["--classic", hierarchy]
                    ),
                )
            )
            for guest in ("ept_entry", "vmx_nmi"):
                cases.append(
                    (
                        f"{guest}-{hierarchy}",
                        "ruby.py",
                        ["--cpu", "timing", "--guest", guest]
                        + (
                            []
                            if hierarchy == "ruby"
                            else ["--classic", hierarchy]
                        ),
                    )
                )
        for guest in ("ept_entry", "vmx_nmi"):
            cases.append(
                (
                    f"{guest}-ruby-o3",
                    "ruby.py",
                    ["--cpu", "o3", "--guest", guest],
                )
            )
        # An unsupported topology is a diagnostic test, never a guest pass.
        for cpu in ("atomic", "timing", "minor", "o3"):
            for broadcast in (False, True):
                cases.append(
                    (
                        f"raw-swap-{cpu}-{int(broadcast)}",
                        "config.py",
                        [
                            "--cpu",
                            cpu,
                            "--guest",
                            "ept_entry",
                            "--caches",
                            "--direct-walker",
                        ]
                        + (["--no-snoop-filter"] if broadcast else []),
                    )
                )
    for name, script, options in cases:
        output = args.outdir / name
        output.mkdir(parents=True, exist_ok=True)
        command = [
            str(args.gem5),
            f"--outdir={output}",
            str(suite / script),
            *options,
        ]
        start = time.monotonic()
        with (output / "console.log").open("w") as log:
            try:
                status = subprocess.run(
                    command,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=args.timeout,
                ).returncode
            except subprocess.TimeoutExpired:
                status = "timeout"
        process_status = status
        if name.startswith("raw-swap-"):
            diagnostic = (output / "console.log").read_text()
            expected = (
                isinstance(status, int)
                and status != 0
                and "unsupported raw SwapReq snoop" in diagnostic
                and "Assertion" not in diagnostic
            )
            status = 0 if expected else status or 1
        elif name == "vmx-address-narrowed-restore":
            diagnostic = (output / "console.log").read_text()
            expected = (
                isinstance(status, int)
                and status != 0
                and any(
                    message in diagnostic
                    for message in (
                        "Malformed VMX checkpoint: invalid VMCS region pointer",
                        "Malformed VMX checkpoint: invalid VMXON pointer",
                    )
                )
            )
            status = 0 if expected else status or 1
        results.append(
            {
                "case": name,
                "status": status,
                "process_status": process_status,
                "command": command,
                "seconds": round(time.monotonic() - start, 3),
            }
        )
        (args.outdir / "results.json").write_text(
            json.dumps(results, indent=2)
        )
        print(
            f'{name}: {"PASS" if status == 0 else f"FAIL ({status})"}',
            flush=True,
        )
    return int(any(result["status"] != 0 for result in results))


if __name__ == "__main__":
    raise SystemExit(main())
