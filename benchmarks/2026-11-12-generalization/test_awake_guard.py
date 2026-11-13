#!/usr/bin/env python3
"""Exercise environmental invalidation without encoding any benchmark inputs."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import awake_guard as guard


SLEEP = "2026-11-13 06:15:22 -0500 Sleep               \tEntering Sleep state due to 'Idle Sleep':TCPKeepAlive=active Using AC (Charge:100%) 937 secs"
WAKE = "2026-11-13 06:30:59 -0500 DarkWake            \tDarkWake from Deep Idle [CDNPB]"


def fake_assertion():
    return subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']), 'test assertion'


class GuardTests(unittest.TestCase):
    def test_recorded_sleep_recognized_without_confusing_wake(self):
        self.assertEqual(guard.sleep_events(SLEEP + '\n' + WAKE + '\n' + SLEEP), [SLEEP])

    def test_sleep_gap_and_clock_changes(self):
        self.assertTrue(guard.heartbeat_violations(938, 938))
        self.assertTrue(guard.heartbeat_violations(938, 1))
        self.assertTrue(guard.heartbeat_violations(-1, 1))
        self.assertTrue(guard.heartbeat_violations(1, -1))
        self.assertTrue(guard.heartbeat_violations(4, 1))
        self.assertFalse(guard.heartbeat_violations(1.2, 1.2))

    def exercise(self, before='', after='', command=None, heartbeat=None, assertion=None):
        with tempfile.TemporaryDirectory(prefix='slippack-awake-test-') as directory:
            root = Path(directory)
            command = command or [sys.executable, '-c', 'print("synthetic benchmark")']
            with patch.object(guard, 'start_assertion', assertion or fake_assertion), \
                    patch.object(guard, 'power_log', side_effect=[before, after]), \
                    patch.object(guard, 'heartbeat_violations', heartbeat or guard.heartbeat_violations):
                result = guard.run_guarded(command, root / 'launch.log', root / 'guard.json')
            self.assertEqual(result, json.loads((root / 'guard.json').read_text()))
            self.assertTrue((root / 'launch.log.before-power.log').exists())
            self.assertTrue((root / 'launch.log.after-power.log').exists())
            return result

    def test_success_with_preexisting_sleep(self):
        result = self.exercise(SLEEP, SLEEP + '\n' + WAKE)
        self.assertTrue(result['valid'])
        self.assertEqual(result['new_sleep_events'], [])

    def test_new_sleep_invalidates_even_successful_benchmark(self):
        result = self.exercise('', SLEEP)
        self.assertEqual(result['exit_code'], 0)
        self.assertFalse(result['valid'])
        self.assertEqual(result['new_sleep_events'], [SLEEP])

    def test_nonzero_benchmark_rejected(self):
        result = self.exercise(command=[sys.executable, '-c', 'raise SystemExit(7)'])
        self.assertFalse(result['valid'])
        self.assertEqual(result['exit_code'], 7)

    def test_monitoring_gap_stops_entire_private_process_group(self):
        with tempfile.TemporaryDirectory(prefix='slippack-awake-child-') as directory:
            pid_path = Path(directory) / 'pid'
            child_code = ('import subprocess,sys,time,pathlib; '
                          'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                          f'pathlib.Path({str(pid_path)!r}).write_text(str(p.pid)); time.sleep(60)')
            result = self.exercise(command=[sys.executable, '-c', child_code],
                                   heartbeat=lambda *_: ['injected monitoring gap'])
            self.assertFalse(result['valid'])
            self.assertLess(result['exit_code'], 0)
            pid = int(pid_path.read_text())
            state = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='],
                                   text=True, stdout=subprocess.PIPE).stdout.strip()
            self.assertTrue(not state or state.startswith('Z'), state)

    def test_lost_assertion_invalidates_launch(self):
        def exited_assertion():
            process = subprocess.Popen([sys.executable, '-c', 'pass'])
            process.wait()
            return process, 'already exited test assertion'
        result = self.exercise(assertion=exited_assertion)
        self.assertFalse(result['valid'])
        self.assertTrue(any('sleep-prevention process exited' in reason
                            for item in result['violations'] for reason in item['reasons']))

    def test_power_log_failure_is_not_a_pass(self):
        with tempfile.TemporaryDirectory(prefix='slippack-awake-failure-') as directory:
            root = Path(directory)
            with patch.object(guard, 'start_assertion', fake_assertion), \
                    patch.object(guard, 'power_log', side_effect=RuntimeError('unavailable')):
                with self.assertRaises(RuntimeError):
                    guard.run_guarded([sys.executable, '-c', 'pass'], root / 'log', root / 'report.json')
            report = json.loads((root / 'report.json').read_text())
            self.assertFalse(report['valid'])
            self.assertFalse(report['completed'])

    def test_existing_reports_are_never_overwritten(self):
        with tempfile.TemporaryDirectory(prefix='slippack-awake-immutable-') as directory:
            root = Path(directory)
            (root / 'report').write_text('keep this evidence')
            with self.assertRaises(AssertionError):
                guard.run_guarded([sys.executable, '-c', 'pass'], root / 'log', root / 'report')
            self.assertEqual((root / 'report').read_text(), 'keep this evidence')


if __name__ == '__main__':
    unittest.main(verbosity=2)
