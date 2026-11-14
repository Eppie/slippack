"""Separate decoding throughput and old-reader compatibility from encoding."""
import argparse
import hashlib
import json
import random
import statistics
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
for name in ['manifest', 'baseline', 'candidate', 'baseline-references', 'candidate-references', 'output']:
    parser.add_argument('--'+name, required=True, type=Path)
parser.add_argument('--optimized', type=Path)
parser.add_argument('--reps', type=int, default=3)
args = parser.parse_args()
assert args.reps > 0
args.output.mkdir(parents=True, exist_ok=False)
paths = [Path(p) for p in args.manifest.read_text().splitlines()]
raw = sum(p.stat().st_size for p in paths)
variants = {
    'baseline': (args.baseline, args.baseline_references),
    'candidate': (args.candidate, args.candidate_references),
    'old_reader_new_archive': (args.baseline, args.candidate_references),
}
if args.optimized:
    variants['optimized'] = (args.optimized, args.candidate_references)
report = {'completed': False, 'files': len(paths), 'raw_bytes': raw, 'repetitions': args.reps,
    'method': 'RAM inputs and archives; one worker; warmup then interleaved measured decode-only runs. No encoder initialization or execution in these processes. Exact reconstruction checked outside timers. Old reader also checks new archives.',
    'binary_sha256': {name: hashlib.sha256(binary.read_bytes()).hexdigest() for name, (binary, _) in variants.items()},
    'settings': {}}
processes = {}
try:
    for name, (binary, reference) in variants.items():
        p = subprocess.Popen([str(binary.resolve()), str(args.manifest.resolve()), str(reference.resolve())],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        processes[name] = p
        assert json.loads(p.stdout.readline()) == {'ready': True, 'files': len(paths), 'raw_bytes': raw}
    for layout in ['separate', 'grouped']:
        rows = report['settings'][layout] = {}
        for rep in range(-1, args.reps):
            order = list(variants)
            if rep >= 0:
                random.Random(99511+rep).shuffle(order)
            for name in order:
                p = processes[name]
                p.stdin.write(layout+'\n'); p.stdin.flush()
                line = p.stdout.readline()
                if not line:
                    raise RuntimeError(p.stderr.read())
                result = json.loads(line)
                assert result['files_verified'] == len(paths)
                row = rows.setdefault(name, {'samples': []})
                if rep < 0:
                    row['warmup_seconds'] = result['decode_seconds']
                else:
                    row['samples'].append(result['decode_seconds'])
                    row['decode_median_seconds'] = statistics.median(row['samples'])
                    row['decode_mbps'] = raw/1e6/row['decode_median_seconds']
                (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
        for name, row in rows.items():
            print(layout, name, '%.2f MB/s' % row['decode_mbps'], flush=True)
finally:
    for p in processes.values():
        p.stdin.close()
        assert p.wait(timeout=30) == 0, p.stderr.read()
report['completed'] = True
report['all_replay_bytes_identical'] = True
(args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
