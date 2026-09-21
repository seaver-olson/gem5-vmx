# SPDX-License-Identifier: BSD-3-Clause

"""Coherent transport for x86 page-table descriptor updates."""

from ....isas import ISA
from .caches.mmu_cache import MMUCache


def connect_x86_walker_caches(owner, cpu_id, cpu, downstream):
    """Connect the two x86 walkers through caches; return False for other ISAs.

    An x86 walker issues atomic descriptor updates. A Classic cache must
    obtain ownership before a SwapReq can cross a bus with other caches:
    forwarding a raw SwapReq to cache snoop ports is unsupported. Each walker
    keeps its existing external port and gets a separate 8 KiB MMU cache.
    Ruby provides the corresponding ownership through its sequencer.
    """
    if cpu.get_isa() != ISA.X86:
        return False
    icache = MMUCache(size="8KiB", writeback_clean=False)
    dcache = MMUCache(size="8KiB", writeback_clean=False)
    setattr(owner, f"x86_iwalker_cache{cpu_id}", icache)
    setattr(owner, f"x86_dwalker_cache{cpu_id}", dcache)
    icache.mem_side = downstream
    dcache.mem_side = downstream
    cpu.connect_walker_ports(icache.cpu_side, dcache.cpu_side)
    return True
