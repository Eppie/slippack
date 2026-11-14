#!/usr/bin/env python3
"""Extraction confinement, overwrite semantics, and atomic file installation."""
from pathlib import Path
import os
import struct
import random
import resource
import signal
import subprocess
import tempfile

binary = Path(os.environ.get('SLIPPACK_BINARY', Path(__file__).resolve().parents[1] / 'slippack'))

def run(*args, ok=True, cwd=None, **kwargs):
    result = subprocess.run([str(binary), *map(str, args)], cwd=cwd,
                            capture_output=True, text=True, timeout=20, **kwargs)
    assert result.returncode == (0 if ok else 1), (args, result.returncode, result.stderr)
    return result

def rename_member(archive, name):
    # Fixture is one raw file and no pools: directory begins at offset 14.
    original = struct.unpack_from('<I', archive, 14)[0]
    return archive[:14] + struct.pack('<I', len(name)) + name + archive[18 + original:]

with tempfile.TemporaryDirectory(prefix='slippack-path-test-') as temporary:
    work = Path(temporary)
    (work / 'input').write_bytes(b'replay bytes')
    run('c', '-o', 'one.slpc', 'input', cwd=work)
    # Default names follow expanded input paths; explicit names take precedence.
    (work / 'source').mkdir()
    (work / 'source/game.slp').write_bytes(b'game bytes')
    result = run('c', 'source/game.slp', cwd=work)
    assert 'source/game.slpc' in result.stderr
    run('x', work / 'source/game.slpc', '-d', work / 'default-output')
    assert (work / 'default-output/source/game.slp').read_bytes() == b'game bytes'
    run('c', 'source/game.slp', cwd=work, ok=False)
    run('c', 'source/game.slp', '--overwrite', cwd=work)
    run('c', work / 'input', cwd=work)
    assert (work / 'input.slpc').read_bytes()[:6] == b'SLPC\x01\x01'
    run('c', 'input', 'source/game.slp', cwd=work)
    assert (work / 'archive.slpc').is_file()
    (work / 'single-list').write_text('source/game.slp\n')
    run('c', '@single-list', '--overwrite', cwd=work)
    (work / 'multiple-list').write_text('input\nsource/game.slp\n')
    run('c', '@multiple-list', '--overwrite', cwd=work)
    run('c', '-o', 'chosen-name', 'input', cwd=work)
    assert (work / 'chosen-name').is_file()
    # Even --overwrite cannot turn an implicit output into an input replacement.
    previous = (work / 'input.slpc').read_bytes()
    run('c', 'input.slpc', '--overwrite', cwd=work, ok=False)
    assert (work / 'input.slpc').read_bytes() == previous
    (work / 'alias.slp').write_bytes(b'preserve')
    (work / 'alias.slpc').hardlink_to(work / 'alias.slp')
    run('c', 'alias.slp', '--overwrite', cwd=work, ok=False)
    assert (work / 'alias.slp').read_bytes() == b'preserve'
    run('c', 'archive.slpc', 'input', '--overwrite', cwd=work, ok=False)
    run('c', '-o', '', 'input', cwd=work, ok=False)
    archive = (work / 'one.slpc').read_bytes()
    outside = work / 'outside'
    outside.mkdir()
    sentinel = outside / 'sentinel'
    sentinel.write_bytes(b'keep')
    for index, name in enumerate((b'../outside/sentinel', b'/absolute/file', b'C:/drive',
                                  b'nested/../../outside/sentinel', b'folder\\file',
                                  b'zero\0tail', b'new\nline', b'new\rline')):
        path = work / f'unsafe-{index}.slpc'
        path.write_bytes(rename_member(archive, name))
        output = work / f'unsafe-{index}'
        run('x', path, '-d', output, ok=False)
        assert not output.exists()
        assert sentinel.read_bytes() == b'keep'
    nested = work / 'nested.slpc'
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
    run('c', '-o', 'one.slpc', 'input', cwd=work, ok=False)
    assert (work / 'one.slpc').read_bytes() == archive
    run('c', '-o', 'one.slpc', 'input', '--overwrite', cwd=work)
    assert (work / 'one.slpc').read_bytes() == archive
    # Force a real short write: a failed replacement must preserve the target,
    # remove its temporary file, and return an ordinary CLI error.
    def limit_output():
        resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    (work / 'incompressible').write_bytes(random.Random(702).randbytes(16384))
    run('c', '-o', 'one.slpc', '--overwrite', 'incompressible', cwd=work,
        ok=False, preexec_fn=limit_output)
    assert (work / 'one.slpc').read_bytes() == archive
    assert not list(work.rglob('.slippack-tmp-*'))
    # Source path and stored name are separate; absolute sources are portable.
    run('c', '-o', work / 'absolute.slpc', work / 'input')
    run('x', work / 'absolute.slpc', '-d', work / 'absolute')
    assert (work / 'absolute/input').read_bytes() == b'replay bytes'
    # Multiple workers hitting existing paths must fail promptly, without
    # deadlock or incomplete temporary files left by queued work.
    names = []
    for i in range(100):
        name = f'file-{i}'
        (work / name).write_bytes(bytes([i]) * 4096)
        names.append(name)
    run('c', '-o', 'many.slpc', '-T', 8, *names, cwd=work)
    run('x', work / 'many.slpc', '-d', work / 'many', '-T', 8)
    run('x', work / 'many.slpc', '-d', work / 'many', '-T', 8, ok=False)
    assert not list(work.rglob('.slippack-tmp-*'))
    for i, name in enumerate(names):
        assert (work / 'many' / name).read_bytes() == bytes([i]) * 4096
    for option in ('--max-file-mib', '--max-output-mib', '--max-frame-mib'):
        run('t', work / 'one.slpc', option, 0, ok=False)
    for args in (('-T', '0'), ('-T', '4garbage'), ('-l', '999'), ('--unknown',)):
        run('c', '-o', work / 'invalid.slpc', *args, work / 'input', ok=False)
    assert not (work / 'invalid.slpc').exists()
    # Leading directories can be dropped on extraction. Renamed paths are
    # checked against each other before anything is written.
    for name in ('a/b/one.slp', 'a/c/two.slp', 'a/c/one.slp', 'top.slp'):
        (work / 'tree' / name).parent.mkdir(parents=True, exist_ok=True)
        (work / 'tree' / name).write_bytes(name.encode())
    nested = ['a/b/one.slp', 'a/c/two.slp']
    run('c', '-o', work / 'tree.slpc', *nested, cwd=work / 'tree')
    run('c', '-o', work / 'tree-clash.slpc', *nested, 'a/c/one.slp', 'top.slp', cwd=work / 'tree')
    def files(root):
        return {str(path.relative_to(root)): path.read_bytes()
                for path in root.rglob('*') if path.is_file()}
    run('x', work / 'tree.slpc', '-d', work / 'tree-strip1', '--strip-components', 1)
    assert files(work / 'tree-strip1') == {'b/one.slp': b'a/b/one.slp', 'c/two.slp': b'a/c/two.slp'}
    run('x', work / 'tree.slpc', '-d', work / 'tree-strip2', '--strip-components', 2)
    assert files(work / 'tree-strip2') == {'one.slp': b'a/b/one.slp', 'two.slp': b'a/c/two.slp'}
    run('x', work / 'tree.slpc', '-d', work / 'tree-flat', '--flatten')
    assert files(work / 'tree-flat') == files(work / 'tree-strip2')
    # A selection is renamed the same way and avoids names that would collide.
    run('x', work / 'tree-clash.slpc', '-d', work / 'tree-flat-some', '--flatten', 'a/c/one.slp', 'top.slp')
    assert files(work / 'tree-flat-some') == {'one.slp': b'a/c/one.slp', 'top.slp': b'top.slp'}
    for arguments, message in (
            (('--flatten',), 'collide'),
            (('--strip-components', 2), 'collide'),
            (('--strip-components', 1), 'too few path components'),
            (('--strip-components', 3, 'a/b/one.slp'), 'too few path components'),
            (('--strip-components', 0), 'between 1 and 4096'),
            (('--strip-components', 1, '--flatten'), 'mutually exclusive')):
        result = run('x', work / 'tree-clash.slpc', '-d', work / 'tree-refused', *arguments, ok=False)
        assert message in result.stderr, (arguments, result.stderr)
        assert not (work / 'tree-refused').exists(), arguments
    for mode in ('t', 'hash'):
        assert 'apply to extraction' in run(mode, work / 'tree-clash.slpc', '--flatten', ok=False).stderr
    assert 'apply to extraction' in run(
        'c', '-o', work / 'tree-refused.slpc', '--flatten', work / 'input', ok=False).stderr
    # Progress goes to stderr only: lines when piped and requested, an erased
    # in-place line on a terminal, and nothing when piped by default.
    quiet = run('hash', work / 'many.slpc')
    assert quiet.stderr == 'slippack: hashed 100 files\n'
    loud = run('hash', work / 'many.slpc', '--progress')
    assert loud.stdout == quiet.stdout
    lines = loud.stderr.splitlines()
    assert lines[-1] == 'slippack: hashed 100 files' and len(lines) >= 2
    assert all(line.startswith('slippack: hashed ') and line.endswith(' MB') and '/100 files, ' in line
               for line in lines[:-1]), loud.stderr
    assert '\r' not in loud.stderr and '\033' not in loud.stderr
    assert '/100 files' in run('x', work / 'many.slpc', '-d', work / 'many-loud', '--progress').stderr
    assert files(work / 'many-loud') == files(work / 'many')
    packing = run('c', '-o', work / 'many-loud.slpc', '--progress', *names, cwd=work)
    assert 'slippack: read ' in packing.stderr and '/100 files' in packing.stderr
    assert (work / 'many-loud.slpc').read_bytes() == (work / 'many.slpc').read_bytes()
    # Verbose extraction and testing print each file's size and written path
    # on standard output, in completion order, and leave the summary on stderr.
    listing = sorted(run('l', work / 'many.slpc').stdout.splitlines())
    for flag in ('-v', '--verbose'):
        result = run('t', work / 'many.slpc', flag)
        assert sorted(result.stdout.splitlines()) == listing
        assert result.stderr == 'slippack: tested 100 files\n'
    result = run('x', work / 'many.slpc', '-d', work / 'many-verbose', '-v', 'file-3', 'file-70')
    assert sorted(result.stdout.splitlines()) == ['4096  file-3', '4096  file-70']
    result = run('x', work / 'tree.slpc', '-d', work / 'tree-verbose', '-v', '--flatten')
    assert sorted(result.stdout.splitlines()) == ['11  one.slp', '11  two.slp']
    assert files(work / 'tree-verbose') == files(work / 'tree-flat')
    assert run('t', work / 'many.slpc').stdout == ''
    assert run('hash', work / 'many.slpc', '-v').stdout == quiet.stdout
    import pty
    for extra, expected in ((), True), (('--no-progress',), False):
        controller, terminal = pty.openpty()
        with open(work / 'terminal-stdout', 'wb') as captured:
            process = subprocess.Popen([str(binary), 'hash', str(work / 'many.slpc'), *extra],
                                       stdout=captured, stderr=terminal)
        os.close(terminal)
        shown = b''
        while True:  # the controller reports EOF or EIO once the process exits
            try:
                chunk = os.read(controller, 65536)
            except OSError:
                break
            if not chunk:
                break
            shown += chunk
        os.close(controller)
        process.wait(timeout=20)
        output = (work / 'terminal-stdout').read_bytes()
        assert process.returncode == 0 and output.decode() == quiet.stdout
        assert (b'/100 files' in shown) == expected, shown
        # The summary follows an erase, so no progress text remains on its line.
        assert shown.replace(b'\r\n', b'\n').endswith(
            (b'\r\x1b[K' if expected else b'') + b'slippack: hashed 100 files\n'), shown
print('extraction: traversal, symlinks, hardlinks, overwrite, atomic output, limits, writer failures,\n'
      'renamed extraction paths, verbose output, and progress reporting passed')
