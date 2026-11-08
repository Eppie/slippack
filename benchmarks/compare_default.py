#!/usr/bin/env python3
"""Compare the original SLPZ default with current legacy and custom backends."""
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
p.add_argument('--baseline', type=Path, required=True, help='Original v9 binary, before --codec existed')
p.add_argument('--binary', type=Path, default=Path(__file__).resolve().parents[1] / 'slpz')
p.add_argument('--root', type=Path, required=True)
p.add_argument('--list', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--threads', type=int, default=16)
p.add_argument('--repeats', type=int, default=9)
p.add_argument('--level', type=int, default=3)
a = p.parse_args()
if a.threads < 1 or a.repeats < 1:
    p.error('threads and repeats must be positive')
for name in ('baseline', 'binary', 'root', 'list', 'work'):
    setattr(a, name, getattr(a, name).resolve())
a.work.mkdir(parents=True, exist_ok=True)
names = [n for n in a.list.read_text().splitlines() if n]
expected = sorted(hashlib.sha1((a.root/n).read_bytes()).hexdigest()+'  '+n for n in names)
entries = {'original_default': (a.baseline, []),
           'current_legacy': (a.binary, ['--codec', 'legacy']),
           'custom': (a.binary, ['--codec', 'custom'])}
samples = {mode: {'encode': [], 'decode': []} for mode in entries}
sizes = {}
for rep in range(-1, a.repeats):
    order = list(entries)
    random.Random(63753+rep).shuffle(order)
    for mode in order:
        binary, codec_args = entries[mode]
        archive = a.work / (mode+'.slpz')
        for kind, args in [('encode', ['c', *codec_args, '-l', str(a.level), '-o', str(archive), '@'+str(a.list)]),
                           ('decode', ['t', str(archive)])]:
            start = time.perf_counter()
            subprocess.run([str(binary), *args, '-T', str(a.threads)], cwd=a.root, capture_output=True, check=True)
            elapsed = time.perf_counter()-start
            if rep >= 0:
                samples[mode][kind].append(elapsed)
        if rep == -1:
            result = subprocess.run([str(binary), 'hash', str(archive), '-T', str(a.threads)],
                                    capture_output=True, text=True, check=True)
            if sorted(result.stdout.splitlines()) != expected:
                raise RuntimeError(mode+' hash mismatch')
            sizes[mode] = archive.stat().st_size
            print(f'{mode}: {sizes[mode]:,} bytes; hashes verified', flush=True)
        elif archive.stat().st_size != sizes[mode]:
            raise RuntimeError(mode+' archive size changed between rounds')
    print(f'round {rep} complete', flush=True)
rows = [dict(codec=mode, bytes=sizes[mode], encode=statistics.median(s['encode']),
             decode=statistics.median(s['decode']), samples=s) for mode, s in samples.items()]
report = dict(files=len(names), raw_bytes=sum((a.root/n).stat().st_size for n in names),
              threads=a.threads, level=a.level, repeats=a.repeats, platform=platform.platform(),
              decode_includes_disk_writes=False,
              binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
              baseline_binary_sha256=hashlib.sha256(a.baseline.read_bytes()).hexdigest(),
              input_list_sha256=hashlib.sha256(a.list.read_bytes()).hexdigest(), rows=rows)
(a.work/'results.json').write_text(json.dumps(report, indent=2)+'\n')
for row in rows:
    print(json.dumps({k: v for k, v in row.items() if k != 'samples'}), flush=True)
print(a.work/'results.json')
