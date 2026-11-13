#!/usr/bin/env python3
"""Summarize complete frozen-candidate runs without dropping timing samples."""
import argparse
import json
from pathlib import Path
import random
import statistics


def bootstrap(baseline, candidate):
    """Descriptive paired-repeat interval; does not measure corpus uncertainty."""
    rng = random.Random(624143)
    ratios = []
    for _ in range(10000):
        indexes = [rng.randrange(len(baseline)) for _ in baseline]
        ratios.append(100 * (sum(baseline[i] for i in indexes) /
                             sum(candidate[i] for i in indexes) - 1))
    ratios.sort()
    return [ratios[250], ratios[9750]]


def size_stats(before, after):
    changes = [a - b for b, a in zip(before, after)]
    relative = [100 * (a / b - 1) for b, a in zip(before, after)]
    return dict(files=len(before), baseline_bytes=sum(before), candidate_bytes=sum(after),
                aggregate_change_percent=100 * (sum(after) / sum(before) - 1),
                mean_file_change_percent=statistics.mean(relative),
                shrink=sum(x < 0 for x in changes), equal=changes.count(0),
                grow=sum(x > 0 for x in changes), max_growth_bytes=max(changes),
                max_growth_percent=max(relative))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    assert report['completed'] and not report['power']['changes']
    output = {'report': str(args.report), 'timings': {}}
    for workers in (1, 8):
        b = report['rows'][f'baseline-l3-t{workers}']
        c = report['rows'][f'candidate-l3-t{workers}']
        row = output['timings'][str(workers)] = {}
        for metric in ('encode', 'decode'):
            bs, cs = b[metric + '_seconds'], c[metric + '_seconds']
            assert len(bs) == len(cs) == report['repeats']
            bm, cm = statistics.mean(bs), statistics.mean(cs)
            row[metric] = dict(baseline_seconds=bm, candidate_seconds=cm,
                               baseline_MB_s=report['raw_bytes'] / bm / 1e6,
                               candidate_MB_s=report['raw_bytes'] / cm / 1e6,
                               throughput_gain_percent=100 * (bm / cm - 1),
                               time_reduction_percent=100 * (1 - cm / bm),
                               descriptive_95_percent_interval=bootstrap(bs, cs))
    output['sizes'] = size_stats(b['file_bytes'], c['file_bytes'])
    source_by_hash = {}
    for filename in ('development-inventory.json', 'holdout-inventory.json',
                     'development-round2-inventory.json', 'holdout-round2-inventory.json'):
        inventory = json.loads((Path(__file__).parent / filename).read_text())
        source_by_hash.update({r['sha256']: r['source'] for r in inventory['inputs']})
    groups = {}
    for i, item in enumerate(report['inputs']):
        assert item['sha256'] in source_by_hash, 'input missing from source inventories'
        groups.setdefault(source_by_hash[item['sha256']], []).append(i)
    output['source_sizes'] = {
        name: size_stats([b['file_bytes'][i] for i in indexes],
                         [c['file_bytes'][i] for i in indexes])
        for name, indexes in groups.items()}
    output['interval_note'] = ('Paired bootstrap of timing repeats only; descriptive, '
                               'not a confidence interval for unseen source collections.')
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
