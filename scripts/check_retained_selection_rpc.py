"""Explicit host-runner checks against the actual native selection callbacks."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve(strict=True))
VECTORS = json.loads(Path(sys.argv.pop(1)).read_text())


class RetainedSelectionRpcTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='selection-rpc-')
        self.addCleanup(self.directory.cleanup)
        self.env = dict(os.environ, TBOT_RETAINED_TEST_STATE_PATH=str(
            Path(self.directory.name) / 'selection.record'))
        self.bind = next(v for v in VECTORS['valid'] if v['name'] == 'bind')
        self.release = next(v for v in VECTORS['valid'] if v['name'] == 'release')

    def call(self, name, args=None, success=True):
        result = subprocess.run([BINARY, '--selection-rpc', name],
            input=json.dumps(args or {}), text=True, capture_output=True,
            env=self.env, timeout=10)
        if not success:
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
            return
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_fresh_state_is_unowned_not_ready(self):
        self.assertEqual(self.call('self.lesson_assets.selection_state'), {
            'contractVersion': 'retained-assignment-device.v1',
            'state': 'unowned', 'desiredSelectionRevision': 0})

    def test_bind_returns_the_canonical_receipt_and_survives_process_restart(self):
        receipt = self.call('self.lesson_assets.retained_selection', {'operation': self.bind['operation']})
        self.assertEqual(receipt, self.bind['receipt'])
        self.assertNotIn('ready', receipt)
        self.assertEqual(self.call('self.lesson_assets.selection_state'), receipt)
        self.assertEqual(self.call('self.lesson_assets.retained_selection',
            {'operation': self.bind['operation']}), receipt)

    def test_release_fences_stale_bind_across_process_restart(self):
        self.call('self.lesson_assets.retained_selection', {'operation': self.bind['operation']})
        released = self.call('self.lesson_assets.retained_selection', {'operation': self.release['operation']})
        self.assertEqual(released, self.release['receipt'])
        self.call('self.lesson_assets.retained_selection', {'operation': self.bind['operation']}, success=False)
        self.assertEqual(self.call('self.lesson_assets.selection_state'), released)

    def test_unrelated_tool_and_malformed_selection_are_refused(self):
        self.call('self.lesson_assets.sync_to_sd', success=False)
        self.call('self.lesson_assets.retained_selection', {'operation': {}}, success=False)
        self.assertEqual(self.call('self.lesson_assets.selection_state')['state'], 'unowned')


if __name__ == '__main__':
    unittest.main()
