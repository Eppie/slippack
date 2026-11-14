#!/usr/bin/env python3
"""Refresh the fixed matched codec comparison using the Slippack release build."""
import argparse
import datetime
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import statistics
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ours', type=Path, required=True)
    parser.add_argument('--upstream-work', type=Path, default=ROOT / 'build/single-file-study')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    assert args.repeats > 0
    previous = json.loads((ROOT / 'benchmarks/2026-11-10-single-file/results.json').read_text())
    spec = importlib.util.spec_from_file_location(
        'speed_benchmark', ROOT / 'benchmarks/2026-11-10-encode-speed/benchmark.py')
    speed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(speed)
    executables = {'ours': args.ours.resolve(),
                   'slippc': args.upstream_work / 'slippc',
                   'rust': args.upstream_work / 'rust-target/release/replay-codec-benchmark'}
    hashes = {name: digest(path) for name, path in executables.items()}
    for name, old in [('slippc', 'slippc'), ('rust', 'rust-target/release/replay-codec-benchmark')]:
        assert hashes[name] == previous['scopes']['peppi']['binary_sha256'][old]
    sources = {str(p.relative_to(ROOT)): digest(p)
               for p in sorted([*ROOT.glob('*.cpp'), *ROOT.glob('*.h'), Path(__file__).resolve()])}
    report = dict(completed=False, date=datetime.date.today().isoformat(),
                  machine=previous['machine'], repeats=args.repeats,
                  method='Fixed previously published matched sets; one replay per archive; '
                         'one worker; RAM inputs; one warmup and randomly interleaved measured '
                         'repeats. Full reconstruction and byte equality outside timers.',
                  versions={**previous['versions'],
                            'ours_base_commit': subprocess.check_output(
                                ['git', 'rev-parse', 'HEAD'], text=True).strip()},
                  binary_sha256=hashes, source_sha256=sources, inputs=previous['inputs'],
                  power=dict(initial=speed.power_state(), changes=[]), scopes={})
    stop = threading.Event()

    def watch_power():
        while not stop.wait(1):
            state = speed.power_state()
            if state != report['power']['initial']:
                report['power']['changes'].append(state)

    monitor = threading.Thread(target=watch_power, daemon=True)
    monitor.start()
    checked_inputs = {}

    def save():
        args.output.write_text(json.dumps(report, indent=2) + '\n')

    configurations = {
        'all': [('ours', 3), ('slpz', 3), ('slpz', 12)],
        'slippc': [('ours', 3), ('slippc', 6)],
        'peppi': [('ours', 3), ('peppi-none', 3), ('peppi-zstd', 3),
                  ('peppi-outer', 3), ('peppi-outer', 12)],
    }
    try:
        for scope, configs in configurations.items():
            prior = previous['scopes'][scope]
            manifest = args.upstream_work / (scope + '.txt')
            paths = [Path(line) for line in manifest.read_text().splitlines() if line]
            assert len(paths) == prior['files']
            for path, index in zip(paths, prior['input_indexes']):
                expected = previous['inputs'][index]['sha256']
                assert digest(path) == expected
                checked_inputs[path] = expected
            rows = {}
            report['scopes'][scope] = dict(files=prior['files'], raw_bytes=prior['raw_bytes'],
                                           input_indexes=prior['input_indexes'], results=rows)
            processes = {}
            try:
                for mode, _ in configs:
                    if mode in processes:
                        continue
                    kind = mode if mode in ('ours', 'slippc') else 'rust'
                    command = [str(executables[kind]), str(manifest)]
                    if kind == 'rust':
                        command.append(mode)
                    proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, text=True, bufsize=1)
                    processes[mode] = proc
                    assert json.loads(proc.stdout.readline()) == dict(ready=True, files=len(paths))
                for repeat in range(-1, args.repeats):
                    order = configs.copy()
                    random.Random(81337 + repeat).shuffle(order)
                    for mode, level in order:
                        proc = processes[mode]
                        proc.stdin.write(f'{level} 1\n')
                        proc.stdin.flush()
                        result = json.loads(proc.stdout.readline())
                        assert len(result['files']) == len(paths)
                        assert not any(item['error'] for item in result['files']), result
                        sizes = [item['bytes'] for item in result['files']]
                        key = f'{mode}-l{level}-t1'
                        row = rows.setdefault(key, dict(mode=mode, level=level, threads=1,
                                                        bytes=sum(sizes), file_bytes=sizes,
                                                        encode_seconds=[], decode_seconds=[]))
                        assert sizes == row['file_bytes']
                        assert not report['power']['changes'], 'power setting changed'
                        if repeat >= 0:
                            for operation in ('encode', 'decode'):
                                row[f'{operation}_seconds'].append(result[f'{operation}_seconds'])
                                median = statistics.median(row[f'{operation}_seconds'])
                                row[f'{operation}_median_seconds'] = median
                                row[f'{operation}_MBps'] = prior['raw_bytes'] / 1e6 / median
                            row['packed_percent'] = 100 * row['bytes'] / prior['raw_bytes']
                        save()
                        print(scope, key, 'warmup' if repeat == -1 else repeat + 1,
                              round(result['encode_seconds'], 4),
                              round(result['decode_seconds'], 4), flush=True)
            finally:
                for proc in processes.values():
                    proc.stdin.close()
                    assert proc.wait(timeout=30) == 0
    finally:
        stop.set()
        monitor.join()
        report['power']['final'] = speed.power_state()
        save()
    assert report['power']['final'] == report['power']['initial']
    assert not report['power']['changes']
    for name, path in executables.items():
        assert digest(path) == hashes[name]
    for name, expected in sources.items():
        assert digest(ROOT / name) == expected
    for path, expected in checked_inputs.items():
        assert digest(path) == expected
    report['completed'] = True
    report['all_reconstructed_bytes_match'] = True
    save()
    print('All matched comparisons passed exact reconstruction and fingerprint checks.', flush=True)


if __name__ == '__main__':
    main()
