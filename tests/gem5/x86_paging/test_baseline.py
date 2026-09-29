#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Unit checks for source provenance when Git metadata is unavailable."""

import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from baseline import provenance


class ProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.suite = self.root / 'suite'
        self.suite.mkdir()
        self.binary = self.root / 'gem5.opt'
        self.binary.write_bytes(b'binary')
        for name in ('se-context-1', 'se-context-2'):
            (self.suite / name).write_bytes(name.encode())
        self.archive = self.root / 'source.tar.gz'

    def test_github_sha_only_is_explicitly_unverified(self):
        sha = 'a' * 40
        environment = {'GITHUB_ACTIONS': 'true', 'GITHUB_SHA': sha,
                       'GITHUB_WORKSPACE': str(self.root)}
        unavailable = subprocess.CalledProcessError(128, 'git')
        with patch.dict(os.environ, environment, clear=True), \
                patch('baseline.subprocess.check_output', side_effect=unavailable):
            result = provenance(self.binary, self.archive,
                                root=self.root, suite=self.suite)
        self.assertEqual(result['git_head'], sha)
        self.assertIn('unverified', result['source_state'])
        with tarfile.open(self.archive) as source:
            self.assertEqual(set(source.getnames()), {'HEAD', 'source_state'})
            self.assertEqual(source.extractfile('HEAD').read(), (sha + '\n').encode())

    def test_no_git_outside_github_fails_closed(self):
        unavailable = subprocess.CalledProcessError(128, 'git')
        with patch.dict(os.environ, {}, clear=True), \
                patch('baseline.subprocess.check_output', side_effect=unavailable):
            with self.assertRaisesRegex(RuntimeError, 'Git worktree unavailable'):
                provenance(self.binary, root=self.root, suite=self.suite)

    def test_git_worktree_records_patch_and_untracked_source(self):
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        tracked = self.root / 'tracked.txt'
        tracked.write_text('old\n')
        subprocess.run(['git', 'add', 'tracked.txt'], cwd=self.root, check=True)
        subprocess.run(['git', '-c', 'user.name=Provenance Test',
                        '-c', 'user.email=test@example.invalid',
                        'commit', '-qm', 'initial'], cwd=self.root, check=True)
        tracked.write_text('new\n')
        (self.root / 'new.txt').write_text('untracked\n')
        result = provenance(self.binary, self.archive,
                            root=self.root, suite=self.suite)
        self.assertEqual(result['source_state'], 'git-worktree')
        with tarfile.open(self.archive) as source:
            self.assertTrue({'HEAD', 'tracked.patch', 'untracked/new.txt'}
                            <= set(source.getnames()))
            self.assertIn(b'new', source.extractfile('tracked.patch').read())


if __name__ == '__main__':
    unittest.main()
