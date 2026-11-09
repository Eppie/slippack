#!/usr/bin/env python3
"""Interleaved v14/v15 benchmark; warm cache, source-hash validation, raw timings."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--current', type=Path, required=True)
parser.add_argument('--study', type=Path, required=True)
parser.add_argument('--repeats', type=int, default=31)
parser.add_argument('--threads', type=int, default=16)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
folder = Path(__file__).resolve().parent
binaries = {'v14': args.baseline.resolve(), 'v15': args.current.resolve()}
work = args.study / 'round6-final-benchmark'
work.mkdir(exist_ok=True)
report = {'threads': args.threads, 'repeats': args.repeats, 'level': 3,
          'baseline_commit': 'f8e4e02', 'machine': 'Apple M4 Max',
          'binary_sha256': {v: hashlib.sha256(b.read_bytes()).hexdigest()
                            for v, b in binaries.items()}, 'datasets': {}}
for dataset in ('sample', 'mixed', 'holdout'):
    root = args.study / ('mixed' if dataset == 'mixed' else 'summit')
    filelist = folder / f'{dataset}-files.txt'
    names = filelist.read_text().splitlines()
    expected = sorted(hashlib.sha1((root / n).read_bytes()).hexdigest() + '  ' + n
                      for n in names)
    rows = {v: {'encode_seconds': [], 'decode_seconds': []} for v in binaries}
    for repeat in range(-1, args.repeats):
        order = list(binaries)
        random.Random(6301 + repeat).shuffle(order)
        for version in order:
            binary = binaries[version]
            archive = work / f'{dataset}-{version}.slpz'
            commands = {'encode_seconds': ['c', '--codec', 'custom', '-l', '3',
                                          '-o', str(archive), '@' + str(filelist)],
                        'decode_seconds': ['t', str(archive)]}
            for operation, command in commands.items():
                begin = time.perf_counter()
                subprocess.run([str(binary), *command, '-T', str(args.threads)],
                               cwd=root, capture_output=True, check=True)
                elapsed = time.perf_counter() - begin
                if repeat >= 0:
                    rows[version][operation].append(elapsed)
            if repeat == -1:
                result = subprocess.run([str(binary), 'hash', str(archive), '-T',
                                         str(args.threads)], capture_output=True,
                                        text=True, check=True)
                assert sorted(result.stdout.splitlines()) == expected
                rows[version]['bytes'] = archive.stat().st_size
    for row in rows.values():
        row['encode_median_seconds'] = statistics.median(row['encode_seconds'])
        row['decode_median_seconds'] = statistics.median(row['decode_seconds'])
    report['datasets'][dataset] = {'files': len(names),
        'raw_bytes': sum((root / n).stat().st_size for n in names), **rows}
    print(dataset, {v: {k: row[k] for k in ('bytes', 'encode_median_seconds',
                                          'decode_median_seconds')}
                    for v, row in rows.items()}, flush=True)
report['totals'] = {v: {k: sum(ds[v][k] for ds in report['datasets'].values())
                       for k in ('bytes', 'encode_median_seconds', 'decode_median_seconds')}
                    for v in binaries}
report['reduction_percent'] = {
    k: 100 * (1 - report['totals']['v15'][k] / report['totals']['v14'][k])
    for k in report['totals']['v14']}
args.output.write_text(json.dumps(report, indent=2) + '\n')
print('reduction_percent', report['reduction_percent'], flush=True)
