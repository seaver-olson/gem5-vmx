#!/usr/bin/env python3
"""Check VMRESUME semantics, O3 scheduling and no-EPT entry/exit redirects."""
import argparse
import configparser
from pathlib import Path
import re
import subprocess


def check_trace(output, latency):
    config = configparser.ConfigParser()
    config.read(output / 'config.ini')
    cycle = int(config['system.clk_domain']['clock'])
    ops = [section for section in config.values()
           if section.get('opClass') == 'VmxResume']
    assert len(ops) == 1
    assert int(ops[0]['opLat']) == latency
    assert ops[0]['pipelined'] == 'false'

    issues, resumes, decoded = {}, [], []
    executing = None
    entered = 0
    required_flags = {'IsNonSpeculative', 'IsSerializing', 'IsSerializeAfter',
                      'IsControl', 'IsIndirectControl', 'IsUncondControl'}
    with (output / 'trace.txt').open() as trace:
        for line in trace:
            issue = re.search(r'^(\d+): .*Issuing instruction .*\[sn:(\d+)\]', line)
            if issue:
                tick, sequence = map(int, issue.groups())
                issues[sequence] = tick
            execution = re.search(r'Execute: Processing PC .*\[sn:(\d+)\]', line)
            if execution:
                executing = int(execution[1])
            if 'VMRESUME start VMCS' in line:
                tick = int(line.split(':', 1)[0])
                assert executing in issues
                resumes.append((executing, tick - issues[executing]))
            if 'VMRESUME entered VMX non-root' in line:
                entered += 1
            if re.search(r':\s+vmresume\s+:', line):
                assert ': VmxResume :' in line
                flags = re.search(r'flags=\(([^)]*)\)', line)[1].split('|')
                assert required_flags <= set(flags)
                decoded.append(int(re.search(r'FetchSeq=(\d+)', line)[1]))
            if re.search(r':\s+(vmlaunch|vmcall|vmxoff)\s+:', line):
                assert ': No_OpClass :' in line
    # Three failure paths (#UD, VMfailInvalid, VMfailValid), then two successes.
    assert len(resumes) == 5 and len(decoded) == 5 and entered == 2
    assert len(set(decoded)) == 5
    assert [sequence for sequence, _ in resumes] == decoded
    # Check the observable issue-to-execution interval, not just the
    # functional-unit configuration or the guest's end-to-end runtime.
    expected = latency * cycle
    assert [delay for _, delay in resumes] == [expected] * 5, resumes
    return [delay // cycle for _, delay in resumes]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, default=Path('build/X86/gem5.opt'))
    parser.add_argument('--outdir', type=Path, default=Path('m5out-vmresume'))
    args = parser.parse_args()
    suite = Path(__file__).resolve().parent
    subprocess.run(['make', '-C', str(suite), 'vmresume.elf'], check=True)
    for cpu, latency in [('atomic', None), ('timing', None),
                         ('o3', None), ('o3', 21), ('o3', 41)]:
        output = args.outdir / f'{cpu}-{latency or "default"}'
        output.mkdir(parents=True, exist_ok=True)
        command = [str(args.gem5), f'--outdir={output}',
                   '--debug-flags=IQ,IEW,VMX,Exec,ExecFlags,ExecFetchSeq',
                   '--debug-file=trace.txt', str(suite / 'config.py'),
                   '--guest', 'vmresume', '--cpu', cpu, '--caches']
        if latency is not None:
            command += ['--vmresume-latency', str(latency)]
        with (output / 'console.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=120)
        if cpu == 'o3':
            delays = check_trace(output, latency or 1)
            print(f'{output.name}: PASS; issue-to-execute cycles {delays}')
        else:
            print(f'{output.name}: PASS (architectural behavior only)')


if __name__ == '__main__':
    main()
