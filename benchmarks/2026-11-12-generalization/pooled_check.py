#!/usr/bin/env python3
"""Verify real pooled CLI archives at four levels without touching source archives."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    args = parser.parse_args()
    paths = [Path(p).resolve() for p in args.manifest.read_text().splitlines() if p]
    assert len({p.name for p in paths}) == len(paths), 'ambiguous member names'
    expected = {p.name: hashlib.sha1(p.read_bytes()).hexdigest() for p in paths}
    binaries = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    hashes = {name: hashlib.sha256(p.read_bytes()).hexdigest() for name, p in binaries.items()}
    report = dict(completed=False, files=len(paths), binary_sha256=hashes, rows=[])
    with tempfile.TemporaryDirectory(prefix='pooled-check-', dir=args.work_dir) as temporary:
        for level in (1, 3, 7, 9):
            for name, binary in binaries.items():
                archive = Path(temporary) / f'{name}-{level}.slpc'
                subprocess.run([str(binary), 'c', '-l', str(level), '-T', '8',
                                '-o', str(archive), *map(str, paths)],
                               check=True, capture_output=True)
                result = subprocess.run([str(binary), 'hash', '-T', '8', str(archive)],
                                        check=True, capture_output=True, text=True)
                pairs = [line.split('  ', 1) for line in result.stdout.splitlines()]
                actual = {member: digest for digest, member in pairs}
                assert len(pairs) == len(paths) and actual == expected
                report['rows'].append(dict(variant=name, level=level, bytes=archive.stat().st_size,
                                           archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                                           reconstructed_member_hashes_match=True))
                args.output.write_text(json.dumps(report, indent=2) + '\n')
                print(name, level, archive.stat().st_size, 'verified', flush=True)
    for name, p in binaries.items():
        assert hashlib.sha256(p.read_bytes()).hexdigest() == hashes[name]
    for p in paths:
        assert hashlib.sha1(p.read_bytes()).hexdigest() == expected[p.name]
    report['completed'] = True
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
