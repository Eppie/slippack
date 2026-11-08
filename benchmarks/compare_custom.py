#!/usr/bin/env python3
"""Compare codec backends on identical inputs, verifying decoded SHA-1s.

Example: python3 benchmarks/compare_custom.py --root /tmp/replays --list /tmp/list.txt
Artifacts go to a temporary directory unless --work is supplied.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--list', type=Path, required=True)
p.add_argument('--work', type=Path)
p.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1] / 'slpz')
p.add_argument('--baseline', type=Path, help='Compare a previous custom binary instead of legacy')
p.add_argument('--repeats', type=int, default=5)
p.add_argument('--threads', type=int, default=16)
p.add_argument('--level', type=int, default=3)
a = p.parse_args()
if a.repeats < 1 or a.threads < 1:
    p.error('--repeats and --threads must be positive')
a.root = a.root.resolve(); a.list = a.list.resolve(); a.binary = a.binary.resolve()
a.work = a.work.resolve() if a.work else Path(tempfile.mkdtemp(prefix='slpz-bench-'))
a.work.mkdir(parents=True, exist_ok=True)
names = [n for n in a.list.read_text().splitlines() if n]
expected = sorted(hashlib.sha1((a.root/n).read_bytes()).hexdigest()+'  '+n for n in names)
raw = sum((a.root/n).stat().st_size for n in names)
entries = {mode: (a.binary, mode) for mode in ('legacy','tuned','custom')}
if a.baseline:
    a.baseline = a.baseline.resolve()
    entries = {'previous_custom': (a.baseline, 'custom'), 'tuned': (a.binary, 'tuned'), 'custom': (a.binary, 'custom')}
times = {mode: {'encode': [], 'decode': []} for mode in entries}
for rep in range(-1, a.repeats):
    order = list(times); random.Random(8123+rep).shuffle(order)
    for mode in order:
        archive = a.work / f'{mode}.slpz'
        executable, backend = entries[mode]
        for kind, args in [('encode', ['c','--codec',backend,'-o',str(archive),'-l',str(a.level),'@'+str(a.list)]),
                           ('decode', ['t',str(archive)])]:
            start = time.perf_counter()
            subprocess.run([str(executable),*args,'-T',str(a.threads)], cwd=a.root, capture_output=True, check=True)
            dt = time.perf_counter()-start
            if rep >= 0: times[mode][kind].append(dt)
        if rep == -1:
            result = subprocess.run([str(executable),'hash',str(archive),'-T',str(a.threads)], cwd=a.root,capture_output=True,text=True,check=True)
            if sorted(result.stdout.splitlines()) != expected: raise RuntimeError(f'{mode} hash mismatch')
            print(f'{mode}: {archive.stat().st_size:,} bytes; hashes verified', flush=True)
    print(f'round {rep} complete', flush=True)
rows = []
for mode, measured in times.items():
    row = dict(codec=mode,bytes=(a.work/f'{mode}.slpz').stat().st_size,
               encode=statistics.median(measured['encode']),decode=statistics.median(measured['decode']),samples=measured)
    rows.append(row); print(json.dumps(row), flush=True)
report = dict(files=len(names),raw_bytes=raw,threads=a.threads,level=a.level,repeats=a.repeats,
              platform=platform.platform(),decode_includes_disk_writes=False,
              binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
              input_list_sha256=hashlib.sha256(a.list.read_bytes()).hexdigest(),rows=rows)
if a.baseline: report['baseline_binary_sha256'] = hashlib.sha256(a.baseline.read_bytes()).hexdigest()
(a.work/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print(a.work/'results.json')
