#!/usr/bin/env python3
"""Check that environmental evidence is mandatory for qualification."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from awake_protocol import benchmark_command, check_cohorts, check_guard_reports


PUBLIC = Path(__file__).resolve().parent
ROOT = PUBLIC.parent.parent


def plan_fixture():
    plan = json.loads((PUBLIC / 'candidate8-replication-protocol.json').read_text())
    plan['run_id'] = 'candidate8-awake1'
    plan['environment_guard'] = dict(max_gap_seconds=10.0, max_clock_skew_seconds=2.0,
        heartbeat_seconds=1, sleep_prevention='caffeinate -i -s, scoped to launch and wrapper lifetime',
        rejection='Any detected sleep, heartbeat violation, lost assertion process, or audit failure invalidates the entire attempt; never filter individual samples.')
    for name, cohort in plan['datasets'].items():
        cohort['reports'] = [f"{plan['run_id']}-{name}-round3-{cohort['files']}-launch{i}.json" for i in (1, 2, 3)]
        cohort['guard_reports'] = [name.removesuffix('.json') + '-guard.json' for name in cohort['reports']]
    return plan


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='slippack-awake-protocol-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.public = self.root / 'benchmarks/2026-11-12-generalization'
        self.public.mkdir(parents=True)
        self.work = self.root / 'build/goal-2026-11-12'
        self.work.mkdir(parents=True)
        self.plan = plan_fixture()
        source = (PUBLIC / 'awake_guard.py').read_bytes()
        (self.public / 'awake_guard.py').write_bytes(source)
        cohort = self.plan['datasets']['outside521']
        for i, (name, guard) in enumerate(zip(cohort['reports'], cohort['guard_reports']), 1):
            report = dict(completed=True, valid=True, exit_code=0, violations=[], new_sleep_events=[],
                          heartbeat_count=5, max_heartbeat_gap_seconds=10.0, max_clock_skew_seconds=2.0,
                          max_observed_wall_gap_seconds=1.01, max_observed_monotonic_gap_seconds=1.01,
                          guard_sha256=hashlib.sha256(source).hexdigest(), assertion_pid=123,
                          initial_assertions='pid 123(caffeinate): PreventSystemSleep\npid 123(caffeinate): PreventUserIdleSystemSleep',
                          command=benchmark_command(self.root, self.public, self.plan, 'outside521', name),
                          power_logs_sha256={})
            for side in ('before', 'after'):
                path = self.work / f'candidate8-awake1-outside521-launch{i}.log.{side}-power.log'
                path.write_text('No new sleep events\n')
                report['power_logs_sha256'][side] = hashlib.sha256(path.read_bytes()).hexdigest()
            (self.public / guard).write_text(json.dumps(report))

    def guards(self):
        return check_guard_reports(self.root, self.public, self.plan, 'outside521')

    def change_first(self, **values):
        path = self.public / self.plan['datasets']['outside521']['guard_reports'][0]
        report = json.loads(path.read_text())
        report.update(values)
        path.write_text(json.dumps(report))

    def test_fixed_cohorts_accept_new_report_namespace(self):
        check_cohorts(ROOT, PUBLIC, self.plan)

    def test_original_report_names_cannot_be_reused(self):
        self.plan['datasets']['outside521']['reports'][0] = 'candidate8-outside521-round3-1408-launch1.json'
        with self.assertRaises(AssertionError):
            check_cohorts(ROOT, PUBLIC, self.plan)

    def test_all_three_valid_guards_required(self):
        self.assertEqual(len(self.guards()), 3)
        (self.public / self.plan['datasets']['outside521']['guard_reports'][2]).unlink()
        with self.assertRaises(FileNotFoundError):
            self.guards()

    def test_invalid_guard_rejected(self):
        self.change_first(valid=False)
        with self.assertRaises(AssertionError):
            self.guards()

    def test_wrong_command_rejected(self):
        self.change_first(command=['different', 'benchmark'])
        with self.assertRaises(AssertionError):
            self.guards()

    def test_changed_guard_code_rejected(self):
        (self.public / 'awake_guard.py').write_text('different code')
        with self.assertRaises(AssertionError):
            self.guards()

    def test_missing_assertion_rejected(self):
        self.change_first(initial_assertions='pid 123(caffeinate): PreventSystemSleep')
        with self.assertRaises(AssertionError):
            self.guards()

    def test_monitoring_gap_rejected(self):
        self.change_first(max_observed_wall_gap_seconds=11)
        with self.assertRaises(AssertionError):
            self.guards()

    def test_changed_power_log_rejected(self):
        (self.work / 'candidate8-awake1-outside521-launch1.log.after-power.log').write_text('changed')
        with self.assertRaises(AssertionError):
            self.guards()

    def test_sleep_evidence_rejected_even_with_valid_flag(self):
        path = self.work / 'candidate8-awake1-outside521-launch1.log.after-power.log'
        path.write_text("2026-11-13 06:15:22 -0500 Sleep               \tEntering Sleep state due to 'Idle Sleep' 937 secs\n")
        report_path = self.public / self.plan['datasets']['outside521']['guard_reports'][0]
        report = json.loads(report_path.read_text())
        report['power_logs_sha256']['after'] = hashlib.sha256(path.read_bytes()).hexdigest()
        report_path.write_text(json.dumps(report))
        with self.assertRaises(AssertionError):
            self.guards()


if __name__ == '__main__':
    unittest.main(verbosity=2)
