"""Explicit host-runner checks against the actual native selection callbacks."""
import json
import copy
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

    def unseen_release(self):
        operation = copy.deepcopy(self.release['operation'])
        operation.update(operationId='10000000-0000-0000-0000-000000000009',
                         requestId='10000000-0000-0000-0000-000000000002',
                         desiredSelectionRevision=6)
        return operation

    def test_unobserved_bind_release_after_old_release_is_durable_and_fences_late_bind(self):
        self.call('self.lesson_assets.retained_selection', {'operation': self.bind['operation']})
        self.call('self.lesson_assets.retained_selection', {'operation': self.release['operation']})
        operation = self.unseen_release()
        receipt = self.call('self.lesson_assets.retained_selection', {'operation': operation})
        self.assertEqual(receipt['state'], 'released')
        self.assertEqual(receipt['requestId'], operation['requestId'])
        self.assertEqual(receipt['desiredSelectionRevision'], 6)
        self.assertEqual(self.call('self.lesson_assets.selection_state'), receipt)
        self.assertEqual(self.call('self.lesson_assets.retained_selection', {'operation': operation}), receipt)
        late = copy.deepcopy(self.bind['operation'])
        late.update(requestId=operation['requestId'], desiredSelectionRevision=5)
        self.call('self.lesson_assets.retained_selection', {'operation': late}, success=False)
        late['desiredSelectionRevision'] = 7
        self.call('self.lesson_assets.retained_selection', {'operation': late}, success=False)
        conflict = copy.deepcopy(operation)
        conflict['operationId'] = '20000000-0000-0000-0000-000000000009'
        self.call('self.lesson_assets.retained_selection', {'operation': conflict}, success=False)
        self.assertEqual(self.call('self.lesson_assets.selection_state'), receipt)

    def test_unseen_release_cannot_displace_active_owner_or_change_identity(self):
        self.call('self.lesson_assets.retained_selection', {'operation': self.bind['operation']})
        self.call('self.lesson_assets.retained_selection', {'operation': self.unseen_release()}, success=False)
        self.assertEqual(self.call('self.lesson_assets.selection_state'), self.bind['receipt'])
        self.call('self.lesson_assets.retained_selection', {'operation': self.release['operation']})
        for field in ('deviceId', 'consumerIdentity'):
            operation = self.unseen_release()
            operation[field] = '90000000-0000-0000-0000-000000000009'
            self.call('self.lesson_assets.retained_selection', {'operation': operation}, success=False)
        changed = self.unseen_release()
        changed['requestId'] = self.release['operation']['requestId']
        changed['selection']['lessonRowId'] = '90000000-0000-0000-0000-000000000005'
        self.call('self.lesson_assets.retained_selection', {'operation': changed}, success=False)
        self.assertEqual(self.call('self.lesson_assets.selection_state'), self.release['receipt'])


if __name__ == '__main__':
    unittest.main()
