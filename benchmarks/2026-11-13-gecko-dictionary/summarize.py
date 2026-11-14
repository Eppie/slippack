#!/usr/bin/env python3
"""Summarize the fixed dictionary sample separately from evaluation replays."""
from collections import defaultdict
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
report = json.loads((HERE / 'development.json').read_text())
assert report['completed']
dictionary = json.loads((ROOT / 'dictionaries/gecko-v1.json').read_text())
inventory = json.loads((ROOT / 'benchmarks/2026-11-12-generalization/'
                       'development-round3-inventory.json').read_text())['inputs']
by_hash = {row['sha256']: row for row in inventory}
training = {row['sha256'] for row in dictionary['training_inputs']}
excluded = set(dictionary['excluded_sources'])
paths = (ROOT / 'build/goal-2026-11-12/development-round3.txt').read_text().splitlines()
outside = set((ROOT / 'build/goal-2026-11-12/development-outside-original521.txt')
              .read_text().splitlines())
assert len(paths) == len(report['inputs'])
old = report['rows']['baseline-l3-t1']['file_bytes']
new = report['rows']['candidate-l3-t1']['file_bytes']
assert old == report['rows']['baseline-l3-t8']['file_bytes']
assert new == report['rows']['candidate-l3-t8']['file_bytes']
groups, sources = defaultdict(list), defaultdict(list)
for i, (path, row) in enumerate(zip(paths, report['inputs'])):
    key = row['sha256']
    source = by_hash[key]['source']
    groups['all_development'].append(i)
    groups['dictionary_training' if key in training else 'not_in_dictionary_training'].append(i)
    if source in excluded:
        groups['sources_excluded_from_dictionary'].append(i)
    if path in outside:
        groups['outside_original_521'].append(i)
    sources[source].append(i)


def sizes(indexes):
    before = sum(old[i] for i in indexes)
    after = sum(new[i] for i in indexes)
    return dict(files=len(indexes), original_bytes=sum(report['inputs'][i]['bytes'] for i in indexes),
                baseline_bytes=before, candidate_bytes=after,
                saved_bytes=before-after, reduction_percent=100*(1-after/before),
                smaller_files=sum(new[i] < old[i] for i in indexes),
                larger_files=sum(new[i] > old[i] for i in indexes),
                unchanged_files=sum(new[i] == old[i] for i in indexes))


speed = {}
for threads in (1, 8):
    a, b = (report['rows'][f'{v}-l3-t{threads}'] for v in ('baseline', 'candidate'))
    speed[threads] = {}
    for operation in ('encode', 'decode'):
        before, after = (row[f'{operation}_median_seconds'] for row in (a, b))
        speed[threads][operation] = dict(baseline_median_seconds=before,
                                         candidate_median_seconds=after,
                                         time_change_percent=100*(after/before-1))
comparison = []
for name in ('01ffbfd6ad363aaee817a6ca6b9f8aed049c9481.slp',
             '0c9caeb8fab2376816d4f58700f6e4d53179d8e6.slp',
             'Game_20210715T103147.slp'):
    i = next(i for i, row in enumerate(report['inputs']) if row['name'] == name)
    comparison.append(dict(path=paths[i], original_bytes=report['inputs'][i]['bytes'],
                           baseline_bytes=old[i], candidate_bytes=new[i]))
summary = dict(groups={name: sizes(indexes) for name, indexes in groups.items()},
               per_source={name: sizes(indexes) for name, indexes in sorted(sources.items())},
               speed=speed, comparison=comparison, power=report['power'],
               repeats=report['repeats'],
               scope='Existing development set; dictionary-training exclusions are not a fresh '
                     'holdout for the whole codec. No final holdout files used.',
               exact_reconstruction=True, thread_independent_compressed_bytes=True)
(HERE / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({k: v for k, v in summary.items() if k != 'per_source'}, indent=2))
