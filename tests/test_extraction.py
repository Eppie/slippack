#!/usr/bin/env python3
"""Extraction confinement, overwrite semantics, and atomic file installation."""
from pathlib import Path
import struct
import random
import resource
import signal
import subprocess
import tempfile

binary = Path(__file__).resolve().parents[1] / 'slpz'

def run(*args, ok=True, cwd=None, **kwargs):
    result = subprocess.run([str(binary), *map(str, args)], cwd=cwd,
                            capture_output=True, text=True, timeout=20, **kwargs)
    assert result.returncode == (0 if ok else 1), (args, result.returncode, result.stderr)
    return result

def rename_member(archive, name):
    # Fixture is one raw file and no pools: directory begins at offset 14.
    original = struct.unpack_from('<I', archive, 14)[0]
    return archive[:14] + struct.pack('<I', len(name)) + name + archive[18 + original:]

with tempfile.TemporaryDirectory(prefix='slpz-path-test-') as temporary:
    work = Path(temporary)
    (work / 'input').write_bytes(b'replay bytes')
    run('c', '-o', 'one.slpz', 'input', cwd=work)
    archive = (work / 'one.slpz').read_bytes()
    outside = work / 'outside'
    outside.mkdir()
    sentinel = outside / 'sentinel'
    sentinel.write_bytes(b'keep')
    for index, name in enumerate((b'../outside/sentinel', b'/absolute/file', b'C:/drive',
                                  b'nested/../../outside/sentinel', b'folder\\file',
                                  b'zero\0tail', b'new\nline', b'new\rline')):
        path = work / f'unsafe-{index}.slpz'
        path.write_bytes(rename_member(archive, name))
        output = work / f'unsafe-{index}'
        run('x', path, '-d', output, ok=False)
        assert not output.exists()
        assert sentinel.read_bytes() == b'keep'
    nested = work / 'nested.slpz'
    nested.write_bytes(rename_member(archive, b'nested/sentinel'))
    output = work / 'output'
    output.mkdir()
    (output / 'nested').symlink_to(outside, target_is_directory=True)
    run('x', nested, '-d', output, ok=False)
    assert sentinel.read_bytes() == b'keep'
    (output / 'nested').unlink()
    (output / 'nested').mkdir()
    target = output / 'nested/sentinel'
    target.symlink_to(sentinel)
    for flags in ((), ('--overwrite',)):
        run('x', nested, '-d', output, *flags, ok=False)
        assert target.is_symlink() and sentinel.read_bytes() == b'keep'
    target.unlink()
    target.hardlink_to(sentinel)
    run('x', nested, '-d', output, ok=False)
    assert target.read_bytes() == b'keep'
    run('x', nested, '-d', output, '--overwrite')
    assert target.read_bytes() == b'replay bytes' and sentinel.read_bytes() == b'keep'
    run('x', nested, '-d', output, ok=False)
    assert target.read_bytes() == b'replay bytes'
    # Existing archive creation requires explicit overwrite as well.
    run('c', '-o', 'one.slpz', 'input', cwd=work, ok=False)
    assert (work / 'one.slpz').read_bytes() == archive
    run('c', '-o', 'one.slpz', 'input', '--overwrite', cwd=work)
    assert (work / 'one.slpz').read_bytes() == archive
    # Force a real short write: a failed replacement must preserve the target,
    # remove its temporary file, and return an ordinary CLI error.
    def limit_output():
        resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    (work / 'incompressible').write_bytes(random.Random(702).randbytes(16384))
    run('c', '-o', 'one.slpz', '--overwrite', 'incompressible', cwd=work,
        ok=False, preexec_fn=limit_output)
    assert (work / 'one.slpz').read_bytes() == archive
    assert not list(work.rglob('.slpz-tmp-*'))
    # Source path and stored name are separate; absolute sources are portable.
    run('c', '-o', work / 'absolute.slpz', work / 'input')
    run('x', work / 'absolute.slpz', '-d', work / 'absolute')
    assert (work / 'absolute/input').read_bytes() == b'replay bytes'
    # Multiple workers hitting existing paths must fail promptly, without
    # deadlock or incomplete temporary files left by queued work.
    names = []
    for i in range(100):
        name = f'file-{i}'
        (work / name).write_bytes(bytes([i]) * 4096)
        names.append(name)
    run('c', '-o', 'many.slpz', '-T', 8, *names, cwd=work)
    run('x', work / 'many.slpz', '-d', work / 'many', '-T', 8)
    run('x', work / 'many.slpz', '-d', work / 'many', '-T', 8, ok=False)
    assert not list(work.rglob('.slpz-tmp-*'))
    for i, name in enumerate(names):
        assert (work / 'many' / name).read_bytes() == bytes([i]) * 4096
    for option in ('--max-file-mib', '--max-output-mib', '--max-frame-mib'):
        run('t', work / 'one.slpz', option, 0, ok=False)
    for args in (('-T', '0'), ('-T', '4garbage'), ('-l', '999'), ('--unknown',)):
        run('c', '-o', work / 'invalid.slpz', *args, work / 'input', ok=False)
    assert not (work / 'invalid.slpz').exists()
print('extraction: traversal, symlinks, hardlinks, overwrite, atomic output, limits, and writer failures passed')
