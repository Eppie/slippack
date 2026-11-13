#!/usr/bin/env python3
"""Declare a complete replacement after a documented environmental interruption."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re

from awake_protocol import check_cohorts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--incident', required=True)
    args = parser.parse_args()
    assert re.fullmatch(r'candidate[0-9]+', args.candidate)
    assert re.fullmatch(args.candidate + r'-awake[1-9][0-9]*', args.run_id)
    assert Path(args.incident).name == args.incident
    public = Path(__file__).resolve().parent
    root = public.parent.parent
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    old_path = public / f'{args.candidate}-replication-protocol.json'
    target = public / f'{args.run_id}-replication-protocol.json'
    assert not target.exists()
    plan = json.loads(old_path.read_text())
    incident_path = public / args.incident
    incident = json.loads(incident_path.read_text())
    assert incident['candidate'] == args.candidate and not incident['holdout_candidate_use']
    assert incident['sleep_duration_seconds'] > 0 and incident['recorded_decode_seconds'] > incident['sleep_duration_seconds']
    assert any('Entering Sleep state' in line for line in incident['evidence_lines'])
    for name, expected in incident['sha256'].items():
        assert digest(root / name) == expected, name
    info_path = public / f'{args.candidate}-build-info.json'
    assert digest(info_path) == plan['candidate_build_info_sha256']
    for name, expected in {**json.loads(info_path.read_text())['sha256'], **plan['artifact_sha256']}.items():
        assert digest(root / name) == expected, name
    assert not (public / 'holdout-round3-retirement.json').exists()
    assert not list(public.glob('*-holdout-round3-512-launch*.json'))
    validation_path = public / 'awake-validation.json'
    validation = json.loads(validation_path.read_text())
    assert validation['completed'] and all(row['exit_code'] == 0 for row in validation['checks'])
    for name, expected in validation['artifact_sha256'].items():
        assert digest(root / name) == expected, name
    plan['declared_utc'] = datetime.now(timezone.utc).isoformat()
    plan['run_id'] = args.run_id
    plan['reason'] = 'Complete replacement of an environmentally interrupted attempt; identical frozen codec, profile, baseline and inputs.'
    plan['status_at_declaration'] = 'Original attempt and its contaminated sample are retained. This new sequence has no measurements yet. The round3 holdout remains candidate-unused.'
    plan['supersedes_protocol'] = old_path.name
    plan['supersedes_protocol_sha256'] = digest(old_path)
    plan['environmental_incident'] = incident_path.name
    plan['environmental_incident_sha256'] = digest(incident_path)
    plan['replacement_policy'] = 'The independently logged 937-second host sleep invalidated the original attempt. Restart all three cohorts, with all new 27 samples per cohort and no mixing, adjustment, deletion or replacement of old samples. This is an infrastructure correction, not a retry of a performance failure. Any performance failure in this fixed replacement stops qualification; no additional unchanged-candidate launch is authorized.'
    plan['environment_guard'] = dict(max_gap_seconds=10.0, max_clock_skew_seconds=2.0,
        heartbeat_seconds=1, sleep_prevention='caffeinate -i -s, scoped to launch and wrapper lifetime',
        rejection='Any detected sleep, heartbeat violation, lost assertion process, or audit failure invalidates the entire attempt; never filter individual samples.')
    for name, cohort in plan['datasets'].items():
        cohort['reports'] = [f"{args.run_id}-{name}-round3-{cohort['files']}-launch{i}.json" for i in (1, 2, 3)]
        cohort['guard_reports'] = [name.removesuffix('.json') + '-guard.json' for name in cohort['reports']]
        assert all(not (public / value).exists() for value in cohort['reports'] + cohort['guard_reports'])
        for suffix in ('replication-status.json', 'replication-summary.json'):
            assert not (public / f'{args.run_id}-{name}-{suffix}').exists()
    artifacts = [old_path, incident_path, validation_path]
    artifacts += [public / name for name in ('awake_guard.py', 'awake_protocol.py', 'declare_awake_protocol.py',
                  'replicate_awake_round3.py', 'summarize_awake_round3.py', 'test_awake_guard.py', 'test_awake_protocol.py')]
    artifacts += [root / f'build/goal-2026-11-12/qualify_{args.run_id.replace("-", "_")}.py']
    for path in artifacts:
        plan['artifact_sha256'][str(path.relative_to(root))] = digest(path)
    check_cohorts(root, public, plan)
    with target.open('x') as output:
        json.dump(plan, output, indent=2)
        output.write('\n')
    print('Declared complete sleep-guarded replacement:', args.run_id)


if __name__ == '__main__':
    main()
