# Copyright (c) 2026 Seaver Olson
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import signal
import subprocess

from testlib import *
from testlib.helper import log_call

config_path = joinpath(getcwd(), "config.py")
isas = (constants.x86_tag, constants.all_compiled_tag)

for scenario in ("clear", "eoi", "init"):
    gem5_verify_config(
        name=f"x86-interrupt-{scenario}",
        config=config_path,
        config_args=[scenario],
        verifiers=[verifier.MatchRegex("x86 interrupt semantics passed")],
        valid_isas=isas,
        length=constants.quick_tag,
    )

for cpu, num_cpus in (
    ("atomic", 1),
    ("atomic", 2),
    ("timing", 1),
    ("minor", 1),
    ("o3", 1),
):
    gem5_verify_config(
        name=f"x86-thread-reset-{cpu}-{num_cpus}",
        config=config_path,
        config_args=[
            "thread-reset",
            "--cpu",
            cpu,
            "--num-cpus",
            str(num_cpus),
        ],
        verifiers=[verifier.MatchRegex("x86 interrupt semantics passed")],
        valid_isas=isas,
        length=constants.quick_tag,
    )


def run_unsupported_reset(params):
    """Require the abort; a separate verifier checks its exact diagnostic."""
    fixtures = params.fixtures
    gem5 = fixtures[constants.gem5_binary_fixture_name].path
    outdir = fixtures[constants.tempdir_fixture_name].path
    command = [
        gem5,
        "-d",
        outdir,
        "-re",
        "--silent-redirect",
        config_path,
        "reset",
    ]
    try:
        log_call(params.log, command, time=params.time)
    except subprocess.CalledProcessError as error:
        if error.returncode != -signal.SIGABRT:
            raise AssertionError(
                f"APIC RESET: expected SIGABRT ({-signal.SIGABRT}), "
                f"got return code {error.returncode}; see {outdir}"
            ) from error
    else:
        raise AssertionError("Unsupported APIC RESET unexpectedly succeeded")


# gem5_verify_config requires exit status zero, so this negative case uses
# the same binary/output fixtures with an explicit expected-signal runner.
for host in constants.supported_hosts:
    for variant in constants.supported_variants:
        for isa in isas:
            name = f"x86-interrupt-reset-{isa}-{host}-{variant}"
            diagnostic = verifier.MatchRegex(
                r".*panic: Local APIC RESET is not implemented\.$",
                match_stdout=False,
            )
            TestSuite(
                name=name,
                fixtures=[Gem5Fixture(isa, variant), TempdirFixture()],
                tests=[
                    TestFunction(run_unsupported_reset, name=name),
                    diagnostic.instantiate_test(name),
                ],
                tags=(isa, host, variant, constants.quick_tag),
            )
