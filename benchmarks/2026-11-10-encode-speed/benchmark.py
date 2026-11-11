#!/usr/bin/env python3
"""Interleaved encoder comparison with exact round trips and packed-byte fingerprints."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import random
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--levels', default='1,3,9')
    parser.add_argument('--threads', default='1,8')
    parser.add_argument('--repeats', type=int, default=9)
    args = parser.parse_args()
    levels = list(map(int, args.levels.split(',')))
    threads = list(map(int, args.threads.split(',')))
    if args.repeats < 1 or any(n < 1 or n > 12 for n in levels) or any(n < 1 or n > 256 for n in threads):
        parser.error('invalid repeat count, level, or thread count')
    paths = [Path(line) for line in args.manifest.read_text().splitlines() if line]
    binaries = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    report = {
        'baseline_commit': '7ca9b4ed6ed48cd71e1bec7b0f16e854daeeb1d5',
        'date': datetime.date.today().isoformat(),
        'platform': platform.platform(), 'repeats': args.repeats,
        'machine': (subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()
                    if platform.system() == 'Darwin' else platform.processor() or platform.machine()),
        'method': 'One replay per archive, RAM inputs, one warmup per setting, randomized interleaving, verification and packed SHA-1 outside timers.',
        'binary_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()},
        'source_sha256': {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in
                          ('custom_codec.h', 'benchmarks/2026-11-10-single-file/cpp_runner.cpp')},
        'inputs': [{'name': path.name, 'bytes': path.stat().st_size,
                    'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in paths],
        'rows': {},
    }
    report['raw_bytes'] = sum(row['bytes'] for row in report['inputs'])
    processes = {}
    fingerprints = {}
    try:
        for name, executable in binaries.items():
            process = subprocess.Popen([str(executable), str(args.manifest.resolve())],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
            processes[name] = process
            assert json.loads(process.stdout.readline()) == {'ready': True, 'files': len(paths)}
        for workers in threads:
            configurations = [(name, level) for name in binaries for level in levels]
            for repeat in range(-1, args.repeats):
                order = configurations.copy()
                random.Random(17310 + repeat + workers * 101).shuffle(order)
                for name, level in order:
                    process = processes[name]
                    process.stdin.write(f'{level} {workers}\n')
                    process.stdin.flush()
                    result = json.loads(process.stdout.readline())
                    assert not any(row['error'] for row in result['files']), result
                    sizes = [row['bytes'] for row in result['files']]
                    hashes = [row['packed_sha1'] for row in result['files']]
                    fingerprints.setdefault(level, (sizes, hashes))
                    assert fingerprints[level] == (sizes, hashes), 'compressed bytes differ'
                    key = f'{name}-l{level}-t{workers}'
                    row = report['rows'].setdefault(key, {
                        'variant': name, 'level': level, 'threads': workers,
                        'bytes': sum(sizes), 'file_bytes': sizes, 'packed_sha1': hashes,
                        'encode_seconds': [], 'decode_seconds': []})
                    if repeat >= 0:
                        for metric in ('encode_seconds', 'decode_seconds'):
                            row[metric].append(result[metric])
                            row[metric.replace('_seconds', '_median_seconds')] = statistics.median(row[metric])
                    args.output.write_text(json.dumps(report, indent=2) + '\n')
                    print(key, 'warmup' if repeat == -1 else repeat + 1,
                          round(result['encode_seconds'], 4), round(result['decode_seconds'], 4), flush=True)
    finally:
        for process in processes.values():
            process.stdin.close()
            assert process.wait(timeout=30) == 0
    for name, path in binaries.items():
        assert report['binary_sha256'][name] == hashlib.sha256(path.read_bytes()).hexdigest()
    for name, digest in report['source_sha256'].items():
        assert digest == hashlib.sha256(Path(name).read_bytes()).hexdigest()
    print('All decoded bytes and compressed fingerprints match.', flush=True)


if __name__ == '__main__':
    main()
