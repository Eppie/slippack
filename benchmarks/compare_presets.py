#!/usr/bin/env python3
"""Interleave old/new custom presets in one experiment and verify all hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1]/'slpz')
p.add_argument('--root', type=Path, required=True)
p.add_argument('--list', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--threads', type=int, default=16)
p.add_argument('--repeats', type=int, default=9)
a = p.parse_args()
if a.threads < 1 or a.repeats < 1:
    p.error('threads and repeats must be positive')
for name in ('baseline', 'binary', 'root', 'list', 'work'):
    setattr(a, name, getattr(a, name).resolve())
a.work.mkdir(parents=True, exist_ok=True)
names = [n for n in a.list.read_text().splitlines() if n]
expected = sorted(hashlib.sha1((a.root/n).read_bytes()).hexdigest()+'  '+n for n in names)
entries = [('previous_custom', level, a.baseline) for level in (1, 3, 9)]
entries += [('custom', level, a.binary) for level in (1, 3, 7, 9)]
rows = [dict(codec=mode, level=level, samples={'encode': [], 'decode': []})
        for mode, level, _ in entries]
for rep in range(-1, a.repeats):
    order = list(range(len(entries)))
    random.Random(12073+rep).shuffle(order)
    for i in order:
        mode, level, binary = entries[i]
        archive = a.work/f'{mode}-{level}.slpz'
        operations = [('encode', ['c', '--codec', 'custom', '-l', str(level),
                                   '-o', str(archive), '@'+str(a.list)]),
                      ('decode', ['t', str(archive)])]
        for kind, args in operations:
            start = time.perf_counter()
            subprocess.run([str(binary), *args, '-T', str(a.threads)],
                           cwd=a.root, capture_output=True, check=True)
            elapsed = time.perf_counter()-start
            if rep >= 0:
                rows[i]['samples'][kind].append(elapsed)
        if rep == -1:
            result = subprocess.run([str(binary), 'hash', str(archive), '-T', str(a.threads)],
                                    capture_output=True, text=True, check=True)
            if sorted(result.stdout.splitlines()) != expected:
                raise RuntimeError(f'{mode} level {level}: hash mismatch')
            rows[i]['bytes'] = archive.stat().st_size
    print(f'round {rep} complete', flush=True)
for row in rows:
    for kind in ('encode', 'decode'):
        row[kind] = statistics.median(row['samples'][kind])
    print(json.dumps({k: v for k, v in row.items() if k != 'samples'}), flush=True)
report = dict(files=len(names), raw_bytes=sum((a.root/n).stat().st_size for n in names),
              threads=a.threads, repeats=a.repeats, platform=platform.platform(),
              decode_includes_disk_writes=False, randomized_across_presets=True,
              binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
              baseline_binary_sha256=hashlib.sha256(a.baseline.read_bytes()).hexdigest(),
              input_list_sha256=hashlib.sha256(a.list.read_bytes()).hexdigest(), rows=rows)
(a.work/'results.json').write_text(json.dumps(report, indent=2)+'\n')
print(a.work/'results.json')
