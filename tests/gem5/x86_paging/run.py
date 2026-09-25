#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run the disk-free paging CPU/memory matrix and retain per-case evidence."""
import argparse
import json
from pathlib import Path
import subprocess
import time
from baseline import provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, default=Path('build/X86/gem5.opt'))
    parser.add_argument('--outdir', type=Path, default=Path('m5out-paging-matrix'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--transport-only', action='store_true',
                        help='Run expanded transport fixtures separately from baseline')
    parser.add_argument('--vmx-operands-only', action='store_true')
    args = parser.parse_args()
    suite = Path(__file__).resolve().parent
    subprocess.run(['make', '-C', str(suite)], check=True)
    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / 'provenance.json').write_text(
        json.dumps(provenance(args.gem5, args.outdir / 'source.tar.gz'), indent=2) + '\n')
    cases = []
    for guest in ('paging', 'legacy', 'pae'):
        for cpu in ('atomic', 'timing', 'o3'):
            for caches in (False, True):
                cases.append((f'{guest}-{cpu}-classic-{int(caches)}',
                              'config.py', ['--cpu', cpu, '--guest', guest]
                              + (['--caches'] if caches else [])))
            if cpu != 'atomic':
                cases.append((f'{guest}-{cpu}-ruby', 'ruby.py',
                              ['--cpu', cpu, '--guest', guest]))
    for hierarchy in ('l1', 'private-l2', 'shared-l2'):
        for cpu in ('atomic', 'timing', 'o3'):
            cases.append((f'stdlib-{hierarchy}-{cpu}', 'ruby.py',
                          ['--cpu', cpu, '--classic', hierarchy]))
    checkpoint = args.outdir / 'checkpoint-source' / 'checkpoint'
    cases.extend([
        ('checkpoint-source', 'config.py', ['--cpu', 'timing', '--caches',
                                            '--checkpoint-at', '6025000']),
        ('checkpoint-restore', 'config.py', ['--cpu', 'timing', '--caches',
                                             '--restore', str(checkpoint)]),
    ])
    for cpu in ('atomic', 'timing'):
        for caches in (False, True):
            cases.append((f'fixture-{cpu}-{int(caches)}', 'config.py',
                          ['--cpu', cpu, '--fixture']
                          + (['--caches'] if caches else [])))
    cases.append(('fixture-ruby', 'ruby.py', ['--cpu', 'timing', '--fixture']))
    for hierarchy in ('ruby', 'l1', 'private-l2', 'shared-l2'):
        cases.append((f'fixture-peers-{hierarchy}', 'ruby.py',
                      ['--cpu', 'timing', '--fixture-peers'] +
                      ([] if hierarchy == 'ruby' else ['--classic', hierarchy])))
    for source, target in (('atomic', 'timing'), ('timing', 'o3'), ('o3', 'atomic')):
        cases.append((f'switch-{source}-{target}', 'config.py',
                      ['--cpu', source, '--switch-to', target, '--caches',
                       '--switch-at', '1000000' if source == 'atomic' else '6025000']))
    results = []
    pae_checkpoint = args.outdir / 'pae-checkpoint' / 'checkpoint'
    cases.extend([
        ('pae-checkpoint', 'config.py', ['--cpu', 'timing', '--caches',
                                        '--guest', 'pae', '--pae-checkpoint']),
        ('pae-restore', 'config.py', ['--cpu', 'timing', '--caches',
                                     '--guest', 'pae', '--restore', str(pae_checkpoint)]),
    ])
    for source, target in (('atomic', 'timing'), ('timing', 'o3'), ('o3', 'atomic')):
        cases.append((f'pae-switch-{source}-{target}', 'config.py',
                      ['--cpu', source, '--switch-to', target, '--caches',
                       '--guest', 'pae']))
    cases.append(('se-contexts', 'se.py', []))
    for cpu in ('atomic', 'timing', 'o3'):
        cases.append((f'pcid-{cpu}', 'config.py',
                      ['--cpu', cpu, '--caches', '--pcid']))
    if args.transport_only:
        cases = [(f'transport-classic-{int(caches)}', 'config.py',
                  ['--cpu', 'timing', '--transport'] +
                  (['--caches'] if caches else [])) for caches in (False, True)]
        for hierarchy in ('ruby', 'l1', 'private-l2', 'shared-l2'):
            cases.append((f'transport-{hierarchy}', 'ruby.py',
                          ['--cpu', 'timing', '--fixture-peers', '--transport'] +
                          ([] if hierarchy == 'ruby' else ['--classic', hierarchy])))
    if args.vmx_operands_only:
        cases = []
        for cpu in ('atomic', 'timing', 'o3'):
            for caches in (False, True):
                cases.append((f'vmx-operands-{cpu}-classic-{int(caches)}',
                              'config.py', ['--cpu', cpu, '--guest', 'vmx_operands'] +
                              (['--caches'] if caches else [])))
            if cpu != 'atomic':
                cases.append((f'vmx-operands-{cpu}-ruby', 'ruby.py',
                              ['--cpu', cpu, '--guest', 'vmx_operands']))
    for name, script, options in cases:
        output = args.outdir / name
        output.mkdir(parents=True, exist_ok=True)
        command = [str(args.gem5), f'--outdir={output}',
                   str(suite / script), *options]
        start = time.monotonic()
        with (output / 'console.log').open('w') as log:
            try:
                status = subprocess.run(command, stdout=log,
                                        stderr=subprocess.STDOUT,
                                        timeout=args.timeout).returncode
            except subprocess.TimeoutExpired:
                status = 'timeout'
        results.append({'case': name, 'status': status, 'command': command,
                        'seconds': round(time.monotonic() - start, 3)})
        (args.outdir / 'results.json').write_text(json.dumps(results, indent=2))
        print(f'{name}: {"PASS" if status == 0 else f"FAIL ({status})"}',
              flush=True)
    return int(any(result['status'] != 0 for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
