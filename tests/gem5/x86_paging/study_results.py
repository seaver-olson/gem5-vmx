# Copyright (c) 2026 Seaver Olson, Loyola University Chicago
# SPDX-License-Identifier: BSD-3-Clause
"""Summarize the first (ROI) stats dump and verify matched workloads."""

import configparser
import json
import re
import struct


def report(root):
    for cpu in ("atomic", "timing"):
        off, on = (
            (root / f"split-{cpu}-{n}" / "study.bin").read_bytes()
            for n in (0, 64)
        )
        if off != on or struct.unpack("<5Q", on) != (
            0x3CA5,
            0x801000,
            0x181,
            0x3C,
            0x76,
        ):
            raise ValueError(f"{cpu}: mismatched split-load or remap evidence")

    rows = []
    for pages in (32, 128):
        reference = None
        for arm in ("native", "ept-0", "ept-64"):
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
                guest_entries=tlb.getint("size"),
                combined_entries=tlb.getint("combined_entries"),
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
            if arm == "ept-64":
                expected_hits = loads - pages if pages < 64 else 0
                if row["combined.readHits"] != expected_hits:
                    raise ValueError(f"{out}: unexpected data-side reuse")
                if bool(row["combined.evictions"]) != (pages > 64):
                    raise ValueError(
                        f"{out}: unexpected capacity eviction count"
                    )
            rows.append(row)
    (root / "measurements.json").write_text(json.dumps(rows, indent=2) + "\n")
