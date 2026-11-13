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

from summarize import size_stats


def timing_summary(baseline, candidate, raw_bytes):
    assert len(baseline) == len(candidate) == 3
    assert all(len(b) == len(c) == 9 for b, c in zip(baseline, candidate))
    before = [x for launch in baseline for x in launch]
    after = [x for launch in candidate for x in launch]
    assert raw_bytes > 0 and all(math.isfinite(x) and x > 0 for x in before + after)
    bm, cm = statistics.mean(before), statistics.mean(after)
    rng = random.Random(950318)
    gains = []
    for _ in range(10000):
        bsum = csum = 0.0
        for launch in (rng.randrange(3) for _ in range(3)):
            for repeat in (rng.randrange(9) for _ in range(9)):
                bsum += baseline[launch][repeat]
                csum += candidate[launch][repeat]
        gains.append(100 * (bsum / csum - 1))
    gains.sort()
    return dict(
        measured_repetitions=27,
        baseline_seconds=bm,
        candidate_seconds=cm,
        baseline_MB_s=raw_bytes / bm / 1e6,
        candidate_MB_s=raw_bytes / cm / 1e6,
        throughput_gain_percent=100 * (bm / cm - 1),
        time_reduction_percent=100 * (1 - cm / bm),
        baseline_samples_by_launch=baseline,
        candidate_samples_by_launch=candidate,
        launch_throughput_gains_percent=[
            100 * (sum(b) / sum(c) - 1) for b, c in zip(baseline, candidate)
        ],
        descriptive_hierarchical_95_percent_interval=[gains[250], gains[9750]],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', default='candidate5')
    parser.add_argument('--dataset', choices=('development', 'holdout'), required=True)
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
    info_path = public / f'{args.candidate}-build-info.json'
    assert hashlib.sha256(info_path.read_bytes()).hexdigest() == plan['candidate_build_info_sha256']
    for name, expected in json.loads(info_path.read_text())['sha256'].items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected, name
    assert hashlib.sha256((public / 'holdout-round2-inventory.json').read_bytes()).hexdigest() == plan['holdout_inventory_sha256']
    if args.dataset == 'development':
        assert [p.name for p in args.reports] == plan['reports']
        if 'preexisting_report_sha256' in plan:
            assert hashlib.sha256(args.reports[0].read_bytes()).hexdigest() == plan['preexisting_report_sha256']
    else:
        assert [p.name for p in args.reports] == [f'{args.candidate}-holdout-round2-512-launch{i}.json' for i in (1, 2, 3)]
        development = json.loads((public / f'{args.candidate}-development-replication-summary.json').read_text())
        assert development['qualifies_on_pooled_means']
        assert development['protocol_sha256'] == hashlib.sha256(plan_path.read_bytes()).hexdigest()
        for name, expected in development['report_sha256'].items():
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
    inventory_name = 'development-round2-inventory.json' if args.dataset == 'development' else 'holdout-round2-inventory.json'
    inventory = json.loads((public / inventory_name).read_text())
    sources = {r['sha256']: r['source'] for r in inventory['inputs']}
    assert len(original['inputs']) == len(sources) == (1417 if args.dataset == 'development' else 512)
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
