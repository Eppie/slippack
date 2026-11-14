"""Compare independent codec executables with a common RAM benchmark driver."""
import argparse
import hashlib
import json
import random
import statistics
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--manifest', required=True, type=Path)
parser.add_argument('--baseline', required=True, type=Path)
parser.add_argument('--candidate', required=True, type=Path)
parser.add_argument('--pgo', type=Path)
parser.add_argument('--variant', action='append', default=[], metavar='NAME=EXECUTABLE')
parser.add_argument('--references', type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--reps', type=int, default=3)
args = parser.parse_args()
assert args.reps > 0
args.output.mkdir(parents=True, exist_ok=False)
paths = [Path(line) for line in args.manifest.read_text().splitlines()]
raw = sum(path.stat().st_size for path in paths)
names = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
if args.pgo:
    names['pgo'] = args.pgo.resolve()
for value in args.variant:
    name, path = value.split('=', 1)
    assert name not in names
    names[name] = Path(path).resolve()
references = (args.references or args.output/'references').resolve()
manifest = args.manifest.resolve()
if not args.references:
    writer = subprocess.Popen([str(names['baseline']), str(manifest), str(references), 'write'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert json.loads(writer.stdout.readline())['files'] == len(paths)
    writer.stdin.write('separate\ngrouped\n')
    writer.stdin.close()
    for line in writer.stdout:
        assert json.loads(line)['files_verified'] == len(paths)
    assert writer.wait() == 0

report = {
    'completed': False, 'files': len(paths), 'raw_bytes': raw, 'repetitions': args.reps,
    'method': 'Level 3, one worker, RAM inputs, one warmup and randomly interleaved measured repetitions. Identical driver object, one codec library per executable. Both variants load reference packets through the same path before timing. Archive comparisons, exact replay verification, fingerprints and final result-buffer disposal outside timers.',
    'manifest': str(manifest), 'references': str(references),
    'binary_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in names.items()},
    'driver_sha256': hashlib.sha256(Path(__file__).with_name('cpp_runner.cpp').read_bytes()).hexdigest(),
    'settings': {},
}


def save():
    (args.output/'results.json').write_text(json.dumps(report, indent=2) + '\n')


processes = {}
try:
    for name, binary in names.items():
        print('Loading RAM inputs:', name, flush=True)
        p = subprocess.Popen([str(binary), str(manifest), str(references), 'check'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        processes[name] = p
        ready = json.loads(p.stdout.readline())
        assert ready == {'ready': True, 'files': len(paths), 'raw_bytes': raw}
    for layout in ['separate', 'grouped']:
        variants = report['settings'][layout] = {}
        for rep in range(-1, args.reps):
            order = list(names)
            if rep >= 0:
                random.Random(44321 + rep).shuffle(order)
            for name in order:
                print('Starting', layout, name, 'warmup' if rep < 0 else f'run {rep+1}', flush=True)
                p = processes[name]
                p.stdin.write(layout + '\n')
                p.stdin.flush()
                line = p.stdout.readline()
                if not line:
                    raise RuntimeError(p.stderr.read())
                result = json.loads(line)
                assert result['files_verified'] == len(paths)
                row = variants.setdefault(name, {
                    'compressed_bytes': result['compressed_bytes'], 'samples': [],
                    'archive_bytes_identical_to_reference': True,
                })
                assert result['compressed_bytes'] == row['compressed_bytes']
                row['archive_bytes_identical_to_reference'] &= result['archive_bytes_identical']
                sample = {key: result[key] for key in ['encode_seconds', 'decode_seconds']}
                if rep < 0:
                    row['warmup'] = sample
                else:
                    row['samples'].append(sample)
                    for op in ['encode', 'decode']:
                        median = statistics.median(s[op+'_seconds'] for s in row['samples'])
                        row[op+'_median_seconds'] = median
                        row[op+'_mbps'] = raw / 1e6 / median
                save()
                print('Finished', layout, name, 'c=%.3fs d=%.3fs' %
                    (sample['encode_seconds'], sample['decode_seconds']), flush=True)
        baseline = variants['baseline']
        for name, row in variants.items():
            row['ratio'] = raw / row['compressed_bytes']
            row['size_delta_pct'] = 100 * (row['compressed_bytes']/baseline['compressed_bytes'] - 1)
            for op in ['encode', 'decode']:
                row[op+'_gain_pct'] = 100 * (row[op+'_mbps']/baseline[op+'_mbps'] - 1)
            print(layout, name, 'encode %.2f MB/s (%+.2f%%)' %
                (row['encode_mbps'], row['encode_gain_pct']), flush=True)
finally:
    for p in processes.values():
        p.stdin.close()
        assert p.wait(timeout=30) == 0, p.stderr.read()
for name, path in names.items():
    assert hashlib.sha256(path.read_bytes()).hexdigest() == report['binary_sha256'][name]
report['completed'] = True
report['all_replay_bytes_identical'] = True
save()
print('RESULTS', args.output/'results.json', flush=True)
