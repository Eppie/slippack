#!/usr/bin/env python3
"""Check or regenerate the Clang profile used by make release."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes():
    return {p.name: sha256(p) for p in sorted([*ROOT.glob('*.cpp'), *ROOT.glob('*.h')])}


def compiler_identity(cxx):
    lines = subprocess.check_output([*shlex.split(cxx), '--version'], text=True).splitlines()
    if 'clang' not in lines[0].lower():
        raise ValueError('profile-guided releases require Clang; use make for a portable build')
    return {'version': lines[0], 'target': next(line for line in lines if line.startswith('Target:'))}


def check(args):
    metadata = json.loads(args.profile.with_suffix('.json').read_text())
    if metadata['compiler'] != compiler_identity(args.cxx):
        raise ValueError('profile compiler/target differs; regenerate the profile or use make')
    if metadata['source_sha256'] != source_hashes():
        raise ValueError('profile source is stale; regenerate the profile or use make')
    if metadata['profile_sha256'] != sha256(args.profile):
        raise ValueError('profile checksum differs from its metadata')
    print('Profile matches the compiler, target, and codec sources.')


def train(args):
    identity = compiler_identity(args.cxx)
    sources = source_hashes()
    paths = [Path(line).expanduser().resolve() for line in args.manifest.read_text().splitlines() if line]
    if not paths or any(not p.is_file() for p in paths):
        raise ValueError('manifest must contain existing input files')
    if args.limit < 1:
        raise ValueError('training limit must be positive')
    if len(paths) > args.limit:
        paths = ([paths[0]] if args.limit == 1 else
                 [paths[i * (len(paths) - 1) // (args.limit - 1)] for i in range(args.limit)])
    flags = '-std=c++20 -O3 -Wall -Wextra -fprofile-generate -fprofile-update=atomic'
    build = ROOT / 'build/profile-training'
    subprocess.run(['make', '-B', '-j', str(args.jobs), 'BUILD_DIR=build/profile-training',
                    'PROGRAM=build/profile-training/slippack', 'PROFILE=', 'CXX=' + args.cxx,
                    'CPPFLAGS=', 'LDFLAGS=', 'CXXFLAGS=' + flags, 'all'], cwd=ROOT, check=True)
    inputs = []
    args.profile.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='work-', dir=build) as temporary:
        work = Path(temporary)
        env = os.environ.copy()
        env['LLVM_PROFILE_FILE'] = str(work / 'counters-%m.profraw')
        binary = build / 'slippack'
        for i, path in enumerate(paths):
            archive = work / f'{i}.slpc'
            subprocess.run([str(binary), 'c', '-l', '3', '-T', '1', '-o', str(archive), str(path)],
                           check=True, capture_output=True, env=env)
            result = subprocess.run([str(binary), 'hash', '-T', '1', str(archive)],
                                    check=True, capture_output=True, text=True, env=env)
            data = path.read_bytes()
            expected = hashlib.sha1(data).hexdigest() + '  ' + path.name
            if result.stdout.strip() != expected:
                raise ValueError(f'training round trip failed for input {i}')
            inputs.append({'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            print(f'Verified training input {i + 1}/{len(paths)}', flush=True)
        raw_profiles = sorted(work.glob('*.profraw'))
        if not raw_profiles:
            raise ValueError('compiler did not emit profile counters')
        text = work / 'profile.proftext'
        subprocess.run([*shlex.split(args.llvm_profdata), 'merge', '--text', '-o', str(text),
                        *map(str, raw_profiles)], check=True)
        if source_hashes() != sources:
            raise ValueError('sources changed during profile collection')
        args.profile.write_bytes(text.read_bytes().rstrip(b'\n') + b'\n')
    metadata = {
        'schema': 1, 'date': datetime.date.today().isoformat(),
        'compiler': identity, 'training_flags': flags,
        'training': 'CLI c and hash, level 3, one worker, exact per-file SHA-1 verification',
        'source_sha256': sources, 'profile_sha256': sha256(args.profile),
        'inputs': inputs, 'raw_bytes': sum(p['bytes'] for p in inputs),
    }
    args.profile.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Wrote {args.profile} and its provenance; no source files or existing archives were changed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['check', 'train'])
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--profile', type=Path, default=ROOT / 'profiles/level3.proftext')
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--limit', type=int, default=64)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--llvm-profdata', default=('xcrun llvm-profdata'
                                                  if platform.system() == 'Darwin' else 'llvm-profdata'))
    args = parser.parse_args()
    if args.action == 'train' and (args.manifest is None or args.jobs < 1):
        parser.error('train requires --manifest and a positive --jobs')
    try:
        (check if args.action == 'check' else train)(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
