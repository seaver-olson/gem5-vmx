from gem5.components.boards.x86_board import X86Board
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.isas import ISA
# --- ADD THIS IMPORT ---
from gem5.components.cachehierarchies.classic.no_cache import NoCache

# 1. Setup the hardware
processor = SimpleProcessor(cpu_type=CPUTypes.ATOMIC, isa=ISA.X86, num_cores=1)
memory = DualChannelDDR4_2400(size="2GB")

# --- CHANGE THIS: Use NoCache instead of None ---
cache_hierarchy = NoCache()

board = X86Board(
    clk_freq="3GHz", 
    processor=processor, 
    memory=memory, 
    cache_hierarchy=cache_hierarchy
)

# 2. Set the Workload 
board.set_kernel_disk_workload(
    kernel=obtain_resource("x86-linux-kernel-5.4.49"),
    disk_image=obtain_resource("x86-ubuntu-18.04-img")
)

# 3. Run the simulation
simulator = Simulator(board=board)
simulator.run()
