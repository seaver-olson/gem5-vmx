# Copyright (c) 2026 Seaver Olson, Loyola University Chicago
# SPDX-License-Identifier: BSD-3-Clause
"""Summarize the first (ROI) stats dump and verify matched workloads."""

import configparser
import json
import re
import struct

PAGE_COUNTS = (16, 32, 63, 64, 65, 128)
# Guest entries, combined entries, additional cycles per usable timing hit.
ARM_CONFIGS = {
    "native": (64, 0, 0),
    "ept-0": (64, 0, 0),
    "native-128": (128, 0, 0),
    "ept-0-128": (128, 0, 0),
    "ept-64": (64, 64, 0),
    "ept-64-delay2": (64, 64, 2),
}


def report(root):
    for cpu in ("atomic", "timing", "minor", "o3"):
        off, on, delayed = (
            (root / f"split-{cpu}-{n}" / "study.bin").read_bytes()
            for n in ("0", "64", "64-delay2")
        )
        if (
            off != on
            or on != delayed
            or struct.unpack("<5Q", on)
            != (
                0x3CA5,
                0x801000,
                0x181,
                0x3C,
                0x76,
            )
        ):
            raise ValueError(f"{cpu}: mismatched split-load or remap evidence")

    rows = []
    for pages in PAGE_COUNTS:
        reference = None
        timings = {}
        for arm, capacities in ARM_CONFIGS.items():
            out = root / f"bench-{pages}-{arm}"
            data = (out / "study.bin").read_bytes()
            if reference is not None and reference != data:
                raise ValueError(f"{pages} pages: workload/checksum mismatch")
            reference = data
            size, loads, fingerprint, checksum = struct.unpack("<4Q", data)
            if (size, loads) != (pages, 4096) or not checksum:
                raise ValueError(f"{out}: unexpected work")
            dump = (
                (out / "stats.txt")
                .read_text()
                .split("End Simulation Statistics", 1)[0]
            )
            stats = dict(re.findall(r"^(\S+)\s+(\d+)\s", dump, re.MULTILINE))
            config = configparser.ConfigParser(interpolation=None)
            config.read(out / "config.ini")
            tlb = config["system.cpu.mmu.dtb"]
            guest, combined, hit_cycles = (
                tlb.getint("size"),
                tlb.getint("combined_entries"),
                tlb.getint("combined_hit_latency"),
            )
            if (guest, combined, hit_cycles) != capacities:
                raise ValueError(
                    f"{out}: unexpected translation configuration"
                )
            prefix = "system.cpu.mmu.dtb."
            ticks = int(stats["simTicks"])
            if (
                f"TRANSLATION ROI ticks={ticks}"
                not in (out / "console.log").read_text()
            ):
                raise ValueError(f"{out}: stats and ROI ticks differ")
            row = dict(
                pages=pages,
                arm=arm,
                loads=loads,
                ticks=ticks,
                ticks_per_load=ticks / loads,
                inline_lookup_cycles=0,
                fingerprint=hex(fingerprint),
                checksum=hex(checksum),
                guest_entries=guest,
                combined_entries=combined,
                total_entries=guest + combined,
                combined_hit_cycles=hit_cycles,
                guest_data_read_hits=int(stats[prefix + "rdAccesses"])
                - int(stats[prefix + "rdMisses"]),
            )
            for key in (
                "walker.traffic.guestReads",
                "walker.traffic.eptReads",
                "walker.traffic.guestUpdates",
                "walker.traffic.eptUpdates",
                "walker.traffic.retries",
                "combined.readHits",
                "combined.executeHits",
                "combined.evictions",
                "combined.timingCompletions",
                "combined.timingTicks",
            ):
                row[key] = int(stats[prefix + key])
            # Every loop load must fall between the two drained markers.
            if (
                int(stats[prefix + "rdAccesses"]) + row["combined.readHits"]
                != loads
            ):
                raise ValueError(
                    f"{out}: data translation count differs from loop loads"
                )
            if combined:
                # The shared TLB also holds the loop's instruction page.
                expected_hits = loads - pages if pages < combined else 0
                if row["combined.readHits"] != expected_hits:
                    raise ValueError(f"{out}: unexpected data-side reuse")
                if bool(row["combined.evictions"]) != (pages >= combined):
                    raise ValueError(
                        f"{out}: unexpected capacity eviction count"
                    )
            timings[arm] = ticks
            rows.append(row)
        if timings["ept-64-delay2"] <= timings["ept-64"]:
            raise ValueError(f"{pages} pages: timing hits incurred no delay")
    (root / "measurements.json").write_text(json.dumps(rows, indent=2) + "\n")
