#!/usr/bin/env python3
"""Reject whole benchmark launches interrupted by sleep or lost supervision."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def sleep_events(log):
    return sorted(set(line for line in log.splitlines() if re.match(
        r'^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d [+-]\d{4} Sleep\s+Entering Sleep state', line)))


def heartbeat_violations(wall_delta, monotonic_delta, max_gap=10.0, max_skew=2.0):
    problems = []
    if wall_delta < 0 or monotonic_delta < 0:
        problems.append('clock moved backward')
    if max(wall_delta, monotonic_delta) > max_gap:
        problems.append('heartbeat gap exceeded limit')
    if abs(wall_delta - monotonic_delta) > max_skew:
        problems.append('wall and monotonic clocks diverged')
    return problems


def power_log():
    return subprocess.check_output(['pmset', '-g', 'log'], text=True, timeout=10)


def start_assertion():
    process = subprocess.Popen(['caffeinate', '-i', '-s', '-w', str(os.getpid())])
    try:
        for _ in range(20):
            assert process.poll() is None, 'sleep-prevention process exited'
            assertions = subprocess.check_output(['pmset', '-g', 'assertions'], text=True, timeout=5)
            owned = [line for line in assertions.splitlines()
                     if re.search(r'\bpid\s+' + str(process.pid) + r'\(caffeinate\)', line)]
            if all(any(kind in line for line in owned)
                   for kind in ('PreventSystemSleep', 'PreventUserIdleSystemSleep')):
                return process, assertions
            time.sleep(0.1)
        raise RuntimeError('required scoped sleep-prevention assertions not observed')
    except BaseException:
        process.terminate()
        process.wait(timeout=5)
        raise


def stop_group(process):
    if process is None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    # The driver and every runner share this private process group.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def run_guarded(command, log_path, report_path, max_gap=10.0, max_skew=2.0):
    assert max_gap > 1 and max_skew >= 0
    assert not log_path.exists() and not report_path.exists()
    report = dict(completed=False, valid=False, command=command, violations=[],
                  max_heartbeat_gap_seconds=max_gap, max_clock_skew_seconds=max_skew,
                  heartbeat_interval_seconds=1, heartbeat_count=0,
                  max_observed_wall_gap_seconds=0.0, max_observed_monotonic_gap_seconds=0.0,
                  guard_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    assertion = process = None
    try:
        assertion, assertions = start_assertion()
        report['assertion_pid'] = assertion.pid
        report['initial_assertions'] = assertions
        before = power_log()
        report['initial_sleep_events'] = sleep_events(before)
        report['started_utc'] = datetime.now(timezone.utc).isoformat()
        previous_wall, previous_monotonic = time.time(), time.monotonic()
        with log_path.open('x') as output:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            report['process_group'] = process.pid
            while True:
                try:
                    code = process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    code = None
                wall, monotonic = time.time(), time.monotonic()
                wd, md = wall - previous_wall, monotonic - previous_monotonic
                previous_wall, previous_monotonic = wall, monotonic
                report['heartbeat_count'] += 1
                report['max_observed_wall_gap_seconds'] = max(report['max_observed_wall_gap_seconds'], wd)
                report['max_observed_monotonic_gap_seconds'] = max(report['max_observed_monotonic_gap_seconds'], md)
                problems = heartbeat_violations(wd, md, max_gap, max_skew)
                if assertion.poll() is not None:
                    problems.append('sleep-prevention process exited during launch')
                if problems:
                    report['violations'].append(dict(utc=datetime.now(timezone.utc).isoformat(),
                                                     reasons=problems, wall_delta=wd, monotonic_delta=md))
                    stop_group(process)
                    break
                if code is not None:
                    break
        report['exit_code'] = process.returncode
        after = power_log()
        report['new_sleep_events'] = sorted(set(sleep_events(after)) - set(sleep_events(before)))
        if report['new_sleep_events']:
            report['violations'].append(dict(reasons=['power log records sleep during launch']))
        report['power_logs_sha256'] = {name: hashlib.sha256(value.encode()).hexdigest()
                                      for name, value in [('before', before), ('after', after)]}
        # Logs are evidence outside timers; retain them beside the process output.
        for name, value in [('before', before), ('after', after)]:
            path = log_path.with_name(log_path.name + '.' + name + '-power.log')
            with path.open('x') as output:
                output.write(value)
        if assertion.poll() is not None:
            report['violations'].append(dict(reasons=['sleep-prevention process exited before final audit']))
        report['completed'] = True
        report['valid'] = report['exit_code'] == 0 and not report['violations']
    except BaseException as error:
        report['violations'].append(dict(reasons=[type(error).__name__ + ': ' + str(error)]))
        stop_group(process)
        raise
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        if assertion is not None and assertion.poll() is None:
            assertion.terminate()
            assertion.wait(timeout=5)
        with report_path.open('x') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--max-gap', type=float, default=10.0)
    parser.add_argument('--max-skew', type=float, default=2.0)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    assert command
    result = run_guarded(command, args.log, args.report, args.max_gap, args.max_skew)
    raise SystemExit(0 if result['valid'] else 1)


if __name__ == '__main__':
    main()
