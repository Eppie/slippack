#!/usr/bin/env python3
"""Interleaved encoder comparison with exact round trips and packed-byte fingerprints."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import threading


def power_state():
    if platform.system() != 'Darwin':
        return None
    live = subprocess.check_output(['pmset', '-g', 'live'], text=True, timeout=5)
    battery = subprocess.check_output(['pmset', '-g', 'batt'], text=True, timeout=5)
    mode = re.search(r'powermode\s+(\d+)', live)
    source = re.search(r"Now drawing from '([^']+)'", battery)
    return {'mode': int(mode[1]) if mode else None, 'source': source[1] if source else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--baseline-commit', default='7ca9b4ed6ed48cd71e1bec7b0f16e854daeeb1d5')
    parser.add_argument('--slpz', type=Path, help='Optional upstream slpz benchmark runner')
    parser.add_argument('--power-mode', type=int, help='Require this macOS power mode throughout the run')
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
    if args.slpz:
        binaries['slpz'] = args.slpz.resolve()
    initial_power = power_state()
    if args.power_mode is not None and (initial_power is None or initial_power['mode'] != args.power_mode):
        parser.error('required power mode is not active')
    report = {
        'completed': False,
        'baseline_commit': args.baseline_commit,
        'power': {'initial': initial_power, 'changes': [], 'poll_interval_seconds': 1},
        'date': datetime.date.today().isoformat(),
        'platform': platform.platform(), 'repeats': args.repeats,
        'machine': (subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()
                    if platform.system() == 'Darwin' else platform.processor() or platform.machine()),
        'method': 'One replay per archive, RAM inputs, one warmup per setting, randomized interleaving, verification and packed SHA-1 outside timers.',
        'binary_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()},
        'source_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                          sorted([*Path('.').glob('*.cpp'), *Path('.').glob('*.h'),
                                  Path('benchmarks/2026-11-10-single-file/cpp_runner.cpp'), Path(__file__)])},
        'inputs': [{'name': path.name, 'bytes': path.stat().st_size,
                    'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in paths],
        'rows': {},
    }
    report['raw_bytes'] = sum(row['bytes'] for row in report['inputs'])
    processes = {}
    fingerprints = {}
    stop = threading.Event()
    def monitor_power():
        while not stop.wait(1):
            try:
                current = power_state()
                if current != initial_power:
                    report['power']['changes'].append(current)
            except (OSError, subprocess.SubprocessError) as error:
                report['power']['changes'].append({'monitor_error': str(error)})
    monitor = threading.Thread(target=monitor_power, daemon=True)
    monitor.start()
    try:
        for name, executable in binaries.items():
            process = subprocess.Popen([str(executable), str(args.manifest.resolve())] + (['slpz'] if name == 'slpz' else []),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
            processes[name] = process
            assert json.loads(process.stdout.readline()) == {'ready': True, 'files': len(paths)}
        for workers in threads:
            configurations = [(name, level) for name in binaries for level in levels if name != 'slpz' or level == 3]
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
                    hashes = [row.get('packed_sha1') for row in result['files']]
                    if name != 'slpz':
                        fingerprints.setdefault(level, (sizes, hashes))
                        assert fingerprints[level] == (sizes, hashes), 'compressed bytes differ'
                    assert not report['power']['changes'], 'power setting changed during benchmark'
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
        stop.set()
        monitor.join()
        report['power']['final'] = power_state()
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        for process in processes.values():
            process.stdin.close()
            assert process.wait(timeout=30) == 0
    for name, path in binaries.items():
        assert report['binary_sha256'][name] == hashlib.sha256(path.read_bytes()).hexdigest()
    for name, digest in report['source_sha256'].items():
        assert digest == hashlib.sha256(Path(name).read_bytes()).hexdigest()
    assert not report['power']['changes'] and report['power']['final'] == initial_power, 'power setting changed'
    report['completed'] = True
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('All decoded bytes match; baseline/candidate compressed fingerprints match.', flush=True)


if __name__ == '__main__':
    main()
