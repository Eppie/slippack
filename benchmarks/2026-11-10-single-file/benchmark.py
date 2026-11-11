#!/usr/bin/env python3
"""Single-replay codec benchmark: warm RAM, exact round trips, matched input sets."""
import argparse
import datetime
import hashlib
import json
import platform
from pathlib import Path
import random
import statistics
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

class Runner:
    def __init__(self, mode, manifest, work):
        executable = work / ('slippc' if mode == 'slippc' else 'ours')
        command = [str(executable), str(manifest)]
        if mode not in ('ours', 'slippc'):
            executable = work / 'rust-target/release/replay-codec-benchmark'
            command = [str(executable), str(manifest), mode]
        self.log = (work / f'{manifest.stem}-{mode}.stderr').open('w')
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True, bufsize=1)
        ready = self.process.stdout.readline()
        if not ready or not json.loads(ready)['ready']:
            raise RuntimeError(f'{mode} failed to initialize; see {self.log.name}')

    def run(self, level, threads):
        self.process.stdin.write(f'{level} {threads}\n')
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f'runner stopped; see {self.log.name}')
        return json.loads(line)

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)
        self.log.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--study', type=Path, required=True,
                   help='Existing study directory with summit/ and mixed/ inputs')
    p.add_argument('--work', type=Path, default=ROOT / 'build/single-file-study')
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--per-set', type=int, default=40)
    p.add_argument('--output', type=Path, default=HERE / 'results.json')
    p.add_argument('--preflight-only', action='store_true')
    p.add_argument('--reuse-report', type=Path, help='Reuse coverage and retain scopes not requested')
    p.add_argument('--scopes', default='all,slippc,peppi')
    args = p.parse_args()
    if args.repeats < 1 or args.per_set < 1:
        p.error('--repeats and --per-set must be positive')
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    inputs, paths = [], []
    excluded = []
    for dataset in ('sample', 'mixed', 'holdout'):
        base = args.study / ('mixed' if dataset == 'mixed' else 'summit')
        candidates = (ROOT / f'benchmarks/2026-11-09-speed/{dataset}-files.txt').read_text().splitlines()
        valid = []
        for name in candidates:
            path = base / name
            with path.open('rb') as f:
                header = f.read(1024)
            if not header.startswith(b'{U\x03raw[$U#l') or header[15] != 0x35:
                excluded.append({'dataset': dataset, 'name': name, 'reason': 'not a Slippi replay'})
                continue
            start = 16 + header[16]
            version = list(header[start + 1:start + 4])
            valid.append((name, version))
        # Stable selection, independent of file contents or codec results.
        valid.sort(key=lambda item: hashlib.sha256(item[0].encode()).digest())
        for name, version in valid[:args.per_set]:
            path = (base / name).resolve()
            data = path.read_bytes()
            inputs.append({'dataset': dataset, 'name': name, 'slp_version': version,
                           'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            paths.append(path)
    machine = (subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()
               if platform.system() == 'Darwin' else platform.processor() or platform.machine())
    report = {'date': datetime.date.today().isoformat(), 'machine': machine,
              'platform': platform.platform(), 'repeats': args.repeats,
              'method': 'One replay per compressed file; in-memory library calls; full .slp reconstruction; verification outside timers.',
              'inputs': inputs, 'excluded_non_replays': excluded,
              'versions': {'ours_base_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                           'slpz_commit': subprocess.check_output(['git', '-C', str(ROOT / 'build/alexander-slpz'), 'rev-parse', 'HEAD'], text=True).strip(),
                           'slippc_commit': subprocess.check_output(['git', '-C', str(ROOT / 'build/pcrain-slippc'), 'rev-parse', 'HEAD'], text=True).strip(),
                           'peppi': '2.1.2'},
              'preflight': {}, 'scopes': {}}
    def save():
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    def manifest(label, indexes):
        path = work / f'{label}.txt'
        path.write_text(''.join(str(paths[i]) + '\n' for i in indexes))
        return path
    if args.reuse_report:
        previous = json.loads(args.reuse_report.read_text())
        assert previous['inputs'] == inputs, 'input corpus changed'
        assert previous['repeats'] == args.repeats, 'repeat count changed'
        report = previous
        old_hashes = report.pop('binary_sha256', None)
        if old_hashes:
            for scope in report['scopes'].values():
                scope.setdefault('binary_sha256', old_hashes)
    modes = {'ours': 3, 'slpz': 3, 'slippc': 6, 'peppi-none': 3,
             'peppi-zstd': 3, 'peppi-outer': 3}
    all_manifest = manifest('preflight', range(len(inputs)))
    for mode, level in modes.items():
        if args.reuse_report:
            continue
        runner = Runner(mode, all_manifest, work)
        try:
            result = runner.run(level, 1)
        finally:
            runner.close()
        report['preflight'][mode] = result
        failures = [(i, r['error']) for i, r in enumerate(result['files']) if r['error']]
        print('preflight', mode, 'passed', len(inputs)-len(failures), '/', len(inputs),
              'failures', failures[:5], flush=True)
        save()
    if args.preflight_only:
        return
    valid_for = {m: {i for i, r in enumerate(report['preflight'][m]['files']) if not r['error']}
                 for m in modes}
    all_inputs = valid_for['ours'] & valid_for['slpz']
    scopes = [
        ('all', sorted(all_inputs), {'ours', 'slpz'}),
        ('slippc', sorted(all_inputs & valid_for['slippc']), {'ours', 'slpz', 'slippc'}),
        ('peppi', sorted(all_inputs & valid_for['peppi-outer'] & valid_for['peppi-zstd'] &
                         valid_for['peppi-none']), {'ours', 'slpz', 'peppi-none', 'peppi-zstd', 'peppi-outer'}),
    ]
    configs = [('ours', 1), ('ours', 3), ('ours', 9), ('slpz', 3), ('slpz', 12),
               ('slippc', 6), ('peppi-none', 3), ('peppi-zstd', 3),
               ('peppi-outer', 3), ('peppi-outer', 12)]
    for scope, indexes, allowed in scopes:
        if not indexes or scope not in args.scopes.split(','):
            continue
        subset = manifest(scope, indexes)
        active = [c for c in configs if c[0] in allowed]
        runners = {m: Runner(m, subset, work) for m in dict(active)}
        rows = {}
        report['scopes'][scope] = {'input_indexes': indexes,
                                   'files': len(indexes),
                                   'raw_bytes': sum(inputs[i]['bytes'] for i in indexes),
                                   'results': rows,
                                   'binary_sha256': {name: hashlib.sha256((work / name).read_bytes()).hexdigest()
                                       for name in ('ours', 'slippc', 'rust-target/release/replay-codec-benchmark')}}
        try:
            for threads in (1, 8):
                for repeat in range(-1, args.repeats):
                    order = active.copy()
                    random.Random(17031 + repeat + threads * 101).shuffle(order)
                    for mode, level in order:
                        result = runners[mode].run(level, threads)
                        assert not any(r['error'] for r in result['files']), (scope, mode, result)
                        key = f'{mode}-l{level}-t{threads}'
                        row = rows.setdefault(key, {'mode': mode, 'level': level, 'threads': threads,
                                                     'bytes': sum(r['bytes'] for r in result['files']),
                                                     'file_bytes': [r['bytes'] for r in result['files']],
                                                     'encode_seconds': [], 'decode_seconds': []})
                        assert row['file_bytes'] == [r['bytes'] for r in result['files']]
                        if repeat >= 0:
                            for metric in ('encode_seconds', 'decode_seconds'):
                                row[metric].append(result[metric])
                                row[metric.replace('_seconds', '_median_seconds')] = statistics.median(row[metric])
                        save()
                        print(scope, key, 'warmup' if repeat == -1 else repeat + 1,
                              round(result['encode_seconds'], 4), round(result['decode_seconds'], 4), flush=True)
        finally:
            for runner in runners.values():
                runner.close()
    save()

if __name__ == '__main__':
    main()
