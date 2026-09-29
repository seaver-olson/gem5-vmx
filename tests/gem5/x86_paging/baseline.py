#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Record/compare deterministic no-EPT paging regression measurements.

These signatures cover elapsed simulated ticks, committed instructions/ops,
and TLB access/miss counts. They exclude host speed and are not a calibration
against hardware. Architectural results are checked by run.py's test cases.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile


METRIC = re.compile(
    r'^(simTicks|simInsts|simOps|.*\.mmu\.[id]tb\.(rd|wr|ex)(Accesses|Misses))$'
)


def collect(directory):
    cases = json.loads((directory / 'results.json').read_text())
    result = {}
    for case in cases:
        name = case['case']
        if case['status'] != 0:
            raise ValueError(f'{name}: cannot baseline a failed test')
        if name in result:
            raise ValueError(f'{name}: duplicate test result')
        metrics = {}
        for line in (directory / name / 'stats.txt').read_text().splitlines():
            fields = line.split()
            if len(fields) >= 2 and METRIC.fullmatch(fields[0]):
                # With multiple dumps, retain the final measurement.
                metrics[fields[0]] = fields[1]
        if 'simTicks' not in metrics or not any('.mmu.' in key for key in metrics):
            raise ValueError(f'{name}: incomplete simulator statistics')
        result[name] = metrics
    if not result:
        raise ValueError('Cannot baseline an empty test matrix')
    return result


def provenance(binary, source_archive=None, *, root=None, suite=None):
    if root is None:
        root = Path(__file__).resolve().parents[3]
        workspace = os.environ.get('GITHUB_WORKSPACE')
        if os.environ.get('GITHUB_ACTIONS') == 'true' and workspace:
            checkout = Path(workspace).resolve()
            if Path(__file__).resolve().is_relative_to(checkout):
                root = checkout
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=root,
                                       stderr=subprocess.PIPE)
    digest = hashlib.sha256()
    # Include staged/unstaged tracked edits and new source files. Ignored
    # binaries, checkpoints and output directories are deliberately excluded.
    try:
        in_worktree = git('rev-parse', '--is-inside-work-tree').strip() == b'true'
    except subprocess.CalledProcessError:
        in_worktree = False
    if in_worktree:
        head = git('rev-parse', 'HEAD')
        patch = git('diff', 'HEAD', '--binary')
        digest.update(patch)
        sources = {'tracked.patch': patch, 'HEAD': head}
        source_state = 'git-worktree'
    else:
        # Containerized CI can lack .git after checkout/build. Preserve the
        # checked-out commit identity, but never represent an unverified
        # worktree as a clean one or manufacture an empty tracked.patch.
        sha = os.environ.get('GITHUB_SHA', '')
        workspace = os.environ.get('GITHUB_WORKSPACE', '')
        if (os.environ.get('GITHUB_ACTIONS') != 'true' or
                not re.fullmatch(r'[0-9a-fA-F]{40}', sha) or
                not workspace or Path(workspace).resolve() != root):
            raise RuntimeError(f'Git worktree unavailable at {root}; '
                               'cannot record source provenance')
        head = (sha.lower() + '\n').encode()
        source_state = 'github-sha-only; worktree changes unverified'
        digest.update(source_state.encode() + b'\0' + head)
        sources = {'HEAD': head, 'source_state': source_state.encode() + b'\n'}
    modes = {}
    untracked = (git('ls-files', '--others', '--exclude-standard', '-z')
                 if in_worktree else b'')
    for raw in sorted(untracked.split(b'\0')):
        if raw:
            digest.update(raw + b'\0')
            content = (root / raw.decode()).read_bytes()
            digest.update(content)
            name = 'untracked/' + raw.decode()
            sources[name] = content
            modes[name] = (root / raw.decode()).stat().st_mode & 0o777
    if source_archive is not None:
        with tarfile.open(source_archive, 'w:gz') as archive:
            for name, content in sources.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(content)
                entry.mode = modes.get(name, 0o644)
                archive.addfile(entry, io.BytesIO(content))
    suite = suite or Path(__file__).resolve().parent
    artifacts = [binary, *sorted(suite.glob('*.elf')),
                 suite / 'se-context-1', suite / 'se-context-2']
    hashes = {}
    for path in artifacts:
        artifact_digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                artifact_digest.update(block)
        hashes[str(path)] = artifact_digest.hexdigest()
    return {'git_head': head.decode().strip(), 'source_state': source_state,
            'working_tree_sha256': digest.hexdigest(), 'artifacts_sha256': hashes,
            'source_archive': str(source_archive) if source_archive else None,
            'source_archive_sha256': hashlib.sha256(source_archive.read_bytes()).hexdigest()
            if source_archive else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['record', 'compare'])
    parser.add_argument('--results-dir', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    args = parser.parse_args()
    actual = collect(args.results_dir)
    if args.action == 'record':
        if args.baseline.exists():
            parser.error('Baseline exists; use a new path to preserve the old evidence')
        source = args.results_dir / 'provenance.json'
        origin = json.loads(source.read_text()) if source.exists() else {
            'state': 'Source provenance was not recorded at execution time'}
        args.baseline.write_text(json.dumps({
            'format': 1, 'provenance': origin, 'cases': actual,
        }, indent=2, sort_keys=True) + '\n')
        print(f'Recorded {len(actual)} passing no-EPT cases')
        return 0
    saved = json.loads(args.baseline.read_text())
    if saved.get('format') != 1:
        parser.error('Unsupported baseline format')
    expected = saved['cases']
    differences = []
    for case in sorted(expected.keys() | actual.keys()):
        if case not in expected or case not in actual:
            differences.append(f'{case}: test set changed')
            continue
        before, after = expected[case], actual[case]
        for metric in sorted(before.keys() | after.keys()):
            if before.get(metric) != after.get(metric):
                differences.append(f'{case}: {metric}: '
                                   f'{before.get(metric)} -> {after.get(metric)}')
    if differences:
        print('\n'.join(differences))
        return 1
    print(f'All {len(actual)} no-EPT cases match simulated ticks, instructions, ops and TLB counts')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
