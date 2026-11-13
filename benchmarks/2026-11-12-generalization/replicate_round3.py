#!/usr/bin/env python3
"""Execute a declared three-launch protocol without changing frozen artifacts."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

from round3_cohorts import check_cohorts


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--dataset', choices=('outside521', 'development', 'holdout'), required=True)
    args = parser.parse_args()
    assert re.fullmatch(r'candidate[0-9]+', args.candidate)
    public = Path(__file__).resolve().parent
    root = public.parent.parent
    work = root / 'build/goal-2026-11-12'
    plan_path = public / f'{args.candidate}-replication-protocol.json'
    plan_digest = digest(plan_path)
    plan = json.loads(plan_path.read_text())
    assert plan['candidate'] == args.candidate
    check_cohorts(root, public, plan)
    assert plan['development_launches_total'] == 3
    assert plan['repetitions_per_launch'] == 9
    assert plan['workers'] == [1, 8] and plan['levels'] == [3]
    info_path = public / f'{args.candidate}-build-info.json'
    assert digest(info_path) == plan['candidate_build_info_sha256']
    info = json.loads(info_path.read_text())
    dataset = plan['datasets'][args.dataset]
    inventory_path = public / dataset['inventory']
    inventory_hashes = {r['sha256'] for r in json.loads(inventory_path.read_text())['inputs']}
    assert len(inventory_hashes) == {'outside521': 1408, 'development': 1929, 'holdout': 512}[args.dataset]
    reports = dataset['reports']
    assert len(reports) == len(set(reports)) == 3
    assert all(Path(name).name == name and not (public / name).exists() for name in reports)
    prerequisite_report = None
    prerequisite_digests = {}
    required = [] if args.dataset == 'outside521' else ['outside521']
    if args.dataset == 'holdout':
        required.append('development')
    for prior in required:
        summary_path = public / f'{args.candidate}-{prior}-replication-summary.json'
        summary = json.loads(summary_path.read_text())
        assert summary['candidate'] == args.candidate and summary['dataset'] == prior
        assert summary['completed'] and summary['qualifies_on_pooled_means']
        assert summary['launches'] == 3 and summary['repetitions_per_launch'] == 9
        assert summary['protocol_sha256'] == plan_digest
        assert list(summary['report_sha256']) == plan['datasets'][prior]['reports']
        for name, expected in summary['report_sha256'].items():
            assert digest(public / name) == expected, name
        prerequisite_report = json.loads((public / plan['datasets'][prior]['reports'][0]).read_text())
        prerequisite_digests[prior] = digest(summary_path)

    def check_frozen():
        assert digest(plan_path) == plan_digest
        assert digest(info_path) == plan['candidate_build_info_sha256']
        assert digest(public / 'holdout-round3-inventory.json') == plan['holdout_inventory_sha256']
        for cohort in plan['datasets'].values():
            assert digest(public / cohort['inventory']) == cohort['inventory_sha256']
            assert digest(root / cohort['manifest']) == cohort['manifest_sha256']
        for prior, expected in prerequisite_digests.items():
            assert digest(public / f'{args.candidate}-{prior}-replication-summary.json') == expected
        for name, expected in {**info['sha256'], **plan['artifact_sha256']}.items():
            assert digest(root / name) == expected, name

    check_frozen()
    status_path = public / f'{args.candidate}-{args.dataset}-replication-status.json'
    status = dict(
        started_utc=datetime.now(timezone.utc).isoformat(),
        dataset=args.dataset,
        candidate=args.candidate,
        protocol_sha256=plan_digest,
        prerequisite_summary_sha256=prerequisite_digests,
        completed_launches=[],
        completed=False,
        status='Fixed validation begun; no tuning or changes between launches.',
    )
    with status_path.open('x') as output:
        json.dump(status, output, indent=2)
        output.write('\n')
    reference = None
    for launch, name in enumerate(reports, 1):
        check_frozen()
        output = public / name
        assert not output.exists(), output
        command = [
            'python3', 'benchmarks/2026-11-10-encode-speed/benchmark.py',
            '--baseline', str(work / 'baseline/runner'),
            '--candidate', str(work / args.candidate / 'runner'),
            '--baseline-commit', plan['baseline_commit'],
            '--allow-byte-changes', '--require-ac-power',
            '--manifest', str(root / dataset['manifest']),
            '--levels', '3', '--threads', '1,8', '--repeats', '9',
            '--output', str(output),
        ]
        print(f'Starting {args.dataset} launch {launch}/3', flush=True)
        with (work / f'{args.candidate}-{args.dataset}-launch{launch}.log').open('x') as log:
            subprocess.run(command, cwd=root, check=True, stdout=log, stderr=subprocess.STDOUT)
        result = json.loads(output.read_text())
        assert result['completed'] and result['repeats'] == 9
        assert not result['power']['changes']
        assert len(result['inputs']) == len(inventory_hashes)
        assert {r['sha256'] for r in result['inputs']} == inventory_hashes
        reference = reference or result
        for key in ('inputs', 'source_sha256', 'binary_sha256', 'raw_bytes'):
            assert result[key] == reference[key], (launch, key)
        assert result['power']['initial'] == result['power']['final'] == reference['power']['initial']
        assert result['rows'].keys() == reference['rows'].keys()
        for key, row in result['rows'].items():
            for field in ('packed_sha1', 'file_bytes', 'bytes'):
                assert row[field] == reference['rows'][key][field], (launch, key, field)
        for variant in ('baseline', 'candidate'):
            for field in ('packed_sha1', 'file_bytes', 'bytes'):
                assert result['rows'][f'{variant}-l3-t1'][field] == result['rows'][f'{variant}-l3-t8'][field]
        if prerequisite_report:
            for key in ('source_sha256', 'binary_sha256'):
                assert result[key] == prerequisite_report[key], key
        check_frozen()
        status['completed_launches'].append(launch)
        status_path.write_text(json.dumps(status, indent=2) + '\n')
        print(f'Completed {args.dataset} launch {launch}/3', flush=True)
    subprocess.run([
        'python3', str(public / 'summarize_replicates_round3.py'),
        '--candidate', args.candidate, '--dataset', args.dataset,
        '--reports', *(str(public / name) for name in reports),
        '--output', str(public / f'{args.candidate}-{args.dataset}-replication-summary.json'),
    ], cwd=root, check=True)
    check_frozen()
    status['completed'] = True
    status['finished_utc'] = datetime.now(timezone.utc).isoformat()
    status['status'] = 'All three launches and pooled summary complete; all samples retained.'
    status_path.write_text(json.dumps(status, indent=2) + '\n')


if __name__ == '__main__':
    main()
