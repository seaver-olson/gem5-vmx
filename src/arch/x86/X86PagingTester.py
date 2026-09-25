# SPDX-License-Identifier: BSD-3-Clause
from m5.objects.SimObject import SimObject
from m5.params import Param, RequestPort, ResponsePort
from m5.proxy import Parent


class X86PagingTester(SimObject):
    """Test driver for the real x86 MMU and its configured memory ports."""

    type = "X86PagingTester"
    cxx_header = "arch/x86/paging_tester.hh"
    cxx_class = "gem5::X86ISA::PagingTester"
    system = Param.System(Parent.any, "System containing the idle test CPU")
    transport_cases = Param.Bool(False, "Run additional shared transport cases")
    cpu_side = ResponsePort("Guest walker interception point")
    mem_side = RequestPort("Original walker memory endpoint")
