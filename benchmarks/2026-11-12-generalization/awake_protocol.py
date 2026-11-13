"""Run identities and environmental evidence for a whole-attempt replacement."""
import hashlib
import json
import re

from round3_cohorts import check_cohorts as check_original_cohorts
from awake_guard import sleep_events


def check_cohorts(root, public, plan):
    assert re.fullmatch(r'candidate[0-9]+', plan['candidate'])
    assert re.fullmatch(plan['candidate'] + r'-awake[1-9][0-9]*', plan['run_id'])
    # Preserve every original membership/count/hash rule; only report names differ.
    check_original_cohorts(root, public, {**plan, 'candidate': plan['run_id']})
    assert plan['environment_guard'] == dict(max_gap_seconds=10.0, max_clock_skew_seconds=2.0,
        heartbeat_seconds=1, sleep_prevention='caffeinate -i -s, scoped to launch and wrapper lifetime',
        rejection='Any detected sleep, heartbeat violation, lost assertion process, or audit failure invalidates the entire attempt; never filter individual samples.')
    for cohort in plan['datasets'].values():
        assert cohort['guard_reports'] == [name.removesuffix('.json') + '-guard.json'
                                           for name in cohort['reports']]


def benchmark_command(root, public, plan, dataset, report):
    work = root / 'build/goal-2026-11-12'
    return ['python3', 'benchmarks/2026-11-10-encode-speed/benchmark.py',
            '--baseline', str(work / 'baseline/runner'),
            '--candidate', str(work / plan['candidate'] / 'runner'),
            '--baseline-commit', plan['baseline_commit'], '--allow-byte-changes', '--require-ac-power',
            '--manifest', str(root / plan['datasets'][dataset]['manifest']),
            '--levels', '3', '--threads', '1,8', '--repeats', '9', '--output', str(public / report)]


def check_guard_reports(root, public, plan, dataset):
    checksums = {}
    cohort = plan['datasets'][dataset]
    for launch, (report_name, guard_name) in enumerate(zip(cohort['reports'], cohort['guard_reports'], strict=True), 1):
        path = public / guard_name
        assert path.resolve().parent == public.resolve()
        report = json.loads(path.read_text())
        assert report['completed'] and report['valid'] and report['exit_code'] == 0
        assert not report['violations'] and not report['new_sleep_events']
        assert report['heartbeat_count'] > 0
        assert report['max_heartbeat_gap_seconds'] == plan['environment_guard']['max_gap_seconds']
        assert report['max_clock_skew_seconds'] == plan['environment_guard']['max_clock_skew_seconds']
        assert report['max_observed_wall_gap_seconds'] <= report['max_heartbeat_gap_seconds']
        assert report['max_observed_monotonic_gap_seconds'] <= report['max_heartbeat_gap_seconds']
        assert report['guard_sha256'] == hashlib.sha256((public / 'awake_guard.py').read_bytes()).hexdigest()
        assert report['command'] == benchmark_command(root, public, plan, dataset, report_name)
        owned = [line for line in report['initial_assertions'].splitlines()
                 if re.search(r'\bpid\s+' + str(report['assertion_pid']) + r'\(caffeinate\)', line)]
        assert all(any(kind in line for line in owned)
                   for kind in ('PreventSystemSleep', 'PreventUserIdleSystemSleep'))
        logs = {}
        for side in ('before', 'after'):
            log = root / 'build/goal-2026-11-12' / f"{plan['run_id']}-{dataset}-launch{launch}.log.{side}-power.log"
            raw = log.read_bytes()
            assert hashlib.sha256(raw).hexdigest() == report['power_logs_sha256'][side]
            logs[side] = raw.decode()
        assert not set(sleep_events(logs['after'])) - set(sleep_events(logs['before']))
        checksums[guard_name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return checksums
