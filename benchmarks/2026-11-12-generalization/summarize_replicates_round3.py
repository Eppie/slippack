#!/usr/bin/env python3
"""Summarize the fixed three-launch protocol, retaining all measured samples."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import re
import statistics

from round3_cohorts import check_cohorts
from summarize import size_stats


from summarize_replicates import timing_summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--dataset', choices=('outside521', 'development', 'holdout'), required=True)
    parser.add_argument('--reports', type=Path, nargs=3, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert re.fullmatch(r'candidate[0-9]+', args.candidate)
    assert args.output.resolve() not in [p.resolve() for p in args.reports]
    public = Path(__file__).resolve().parent
    root = public.parent.parent
    plan_path = public / f'{args.candidate}-replication-protocol.json'
    plan = json.loads(plan_path.read_text())
    assert plan['candidate'] == args.candidate
    check_cohorts(root, public, plan)
    info_path = public / f'{args.candidate}-build-info.json'
    assert hashlib.sha256(info_path.read_bytes()).hexdigest() == plan['candidate_build_info_sha256']
    for name, expected in json.loads(info_path.read_text())['sha256'].items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, name
    assert hashlib.sha256((public / 'holdout-round3-inventory.json').read_bytes()).hexdigest() == plan['holdout_inventory_sha256']
    dataset = plan['datasets'][args.dataset]
    assert [p.name for p in args.reports] == dataset['reports']
    assert all(p.resolve().parent == public for p in args.reports)
    assert not args.output.exists(), 'Keep prior qualification summaries immutable'
    for cohort in plan['datasets'].values():
        assert hashlib.sha256((public / cohort['inventory']).read_bytes()).hexdigest() == cohort['inventory_sha256']
        assert hashlib.sha256((root / cohort['manifest']).read_bytes()).hexdigest() == cohort['manifest_sha256']
    required = [] if args.dataset == 'outside521' else ['outside521']
    if args.dataset == 'holdout':
        required.append('development')
    for prior in required:
        prerequisite = json.loads((public / f'{args.candidate}-{prior}-replication-summary.json').read_text())
        assert prerequisite['completed'] and prerequisite['qualifies_on_pooled_means']
        assert prerequisite['candidate'] == args.candidate and prerequisite['dataset'] == prior
        assert prerequisite['launches'] == 3 and prerequisite['repetitions_per_launch'] == 9
        assert prerequisite['protocol_sha256'] == hashlib.sha256(plan_path.read_bytes()).hexdigest()
        assert list(prerequisite['report_sha256']) == plan['datasets'][prior]['reports']
        for name, expected in prerequisite['report_sha256'].items():
            assert hashlib.sha256((public / name).read_bytes()).hexdigest() == expected, name
    for name, expected in plan.get('artifact_sha256', {}).items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, name

    reports = [json.loads(p.read_text()) for p in args.reports]
    original = reports[0]
    for name in ('baseline', 'candidate'):
        for field in ('packed_sha1', 'file_bytes', 'bytes'):
            assert original['rows'][f'{name}-l3-t1'][field] == original['rows'][f'{name}-l3-t8'][field]
    for report in reports:
        assert report['completed'] and report['repeats'] == 9
        assert not report['power']['changes']
        for key in ('inputs', 'source_sha256', 'binary_sha256', 'raw_bytes'):
            assert report[key] == original[key], key
        assert report['power']['initial'] == report['power']['final'] == original['power']['initial']
        assert report['rows'].keys() == original['rows'].keys()
        for key, row in report['rows'].items():
            for field in ('packed_sha1', 'file_bytes', 'bytes'):
                assert row[field] == original['rows'][key][field], (key, field)

    output = dict(
        candidate=args.candidate,
        dataset=args.dataset,
        completed=True,
        launches=3,
        repetitions_per_launch=9,
        files=len(original['inputs']),
        raw_bytes=original['raw_bytes'],
        protocol_sha256=hashlib.sha256(plan_path.read_bytes()).hexdigest(),
        report_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in args.reports},
        timings={},
        power=[r['power'] for r in reports],
        interval_note='Hierarchical bootstrap of three process launches and paired repeats; descriptive only, not a confidence interval over unseen source families.',
    )
    for workers in (1, 8):
        output['timings'][str(workers)] = {}
        for metric in ('encode', 'decode'):
            before = [r['rows'][f'baseline-l3-t{workers}'][metric + '_seconds'] for r in reports]
            after = [r['rows'][f'candidate-l3-t{workers}'][metric + '_seconds'] for r in reports]
            output['timings'][str(workers)][metric] = timing_summary(before, after, original['raw_bytes'])

    b, c = (original['rows'][f'{name}-l3-t1']['file_bytes'] for name in ('baseline', 'candidate'))
    output['sizes'] = size_stats(b, c)
    inventory = json.loads((public / dataset['inventory']).read_text())
    sources = {r['sha256']: r['source'] for r in inventory['inputs']}
    assert len(original['inputs']) == len(sources) == {'outside521': 1408, 'development': 1929, 'holdout': 512}[args.dataset]
    assert set(sources) == {r['sha256'] for r in original['inputs']}
    groups = {}
    for i, item in enumerate(original['inputs']):
        groups.setdefault(sources[item['sha256']], []).append(i)
    output['source_sizes'] = {
        name: size_stats([b[i] for i in indexes], [c[i] for i in indexes])
        for name, indexes in groups.items()
    }
    output['qualifies_on_pooled_means'] = (
        output['sizes']['aggregate_change_percent'] <= 0
        and all(row['encode']['throughput_gain_percent'] >= 10
                and row['decode']['throughput_gain_percent'] >= 0
                for row in output['timings'].values())
    )
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps({k: output[k] for k in ('dataset', 'launches', 'qualifies_on_pooled_means', 'timings', 'sizes')}, indent=2))


if __name__ == '__main__':
    main()
