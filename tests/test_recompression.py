#!/usr/bin/env python3
"""Verify atomic migration, resume, larger-output retention, and hash-failure safety."""
import hashlib
import json
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
binary = repo / 'slpz'
legacy = repo / 'build/legacy-extract'
script = repo / 'tools/recompress_archives.py'
assert legacy.is_file(), 'run python3 tools/build_legacy_extractor.py first'


def run(args, **kw):
    return subprocess.run([str(x) for x in args], capture_output=True, text=True, check=True, **kw)


def replay():
    raw = bytearray([0x35, 7, 0x37, 0, 66, 0x38, 0, 84])
    seed = 12345
    for frame in range(2048):
        for player in range(2):
            pre, post = bytearray(66), bytearray(84)
            for record in (pre, post):
                struct.pack_into('>iBB', record, 0, frame - 123, player, 0)
            seed = (seed * 214013 + 2531011) & 0xffffffff
            struct.pack_into('>I', pre, 6, seed)
            for offset, physical in ((24,58),(28,63),(32,64),(36,65)):
                q = (frame * (player + 1) + offset) % 161 - 80
                pre[physical] = q & 255
                struct.pack_into('>f', pre, offset, q / 80)
            struct.pack_into('>H', post, 7, frame // 47 % 128)
            for offset in (9,13):
                struct.pack_into('>f', post, offset, frame / 4 + player)
            for offset in (52,56,68):
                struct.pack_into('>f', post, offset, .25)
            struct.pack_into('>f', post, 33, float(frame % 47))
            raw += b'\x37' + pre + b'\x38' + post
    return b'{U\x03raw[$U#l' + struct.pack('>I', len(raw)) + raw


with tempfile.TemporaryDirectory(prefix='slpz-migration-test-') as temporary:
    root = Path(temporary)
    source = root / 'source'
    source.mkdir()
    (source / 'game.slp').write_bytes(replay())
    rng = random.Random(4178)
    words = [''.join(rng.choices('abcdefghijklmnopqrstuvwxyz', k=rng.randrange(2, 15)))
             for _ in range(500)]
    (source / 'opaque.bin').write_bytes(' '.join(rng.choices(words, k=20000)).encode())
    archives = root / 'archives'
    archives.mkdir()
    originals = {}
    for name, member in [('game', 'game.slp'), ('opaque', 'opaque.bin')]:
        archive = archives / (name + '.slpz')
        run([legacy, 'c', '-o', archive, '-T', 2, '-l', 19 if name == 'opaque' else 3, member], cwd=source)
        originals[name] = archive.read_bytes()
    state = root / 'state'
    args = ['python3', script, '--root', archives, '--binary', binary, '--extractor', legacy,
            '--state-dir', state, '--threads', 2, '--status-interval', .1, '--apply']
    first = run([*args, '--limit', 1])
    assert 'remaining' in first.stdout and 'MB/s' in first.stdout and 'ETA' in first.stdout
    records = list((state / 'records').glob('*.json'))
    assert len(records) == 1
    first_record = json.loads(records[0].read_text())
    untouched = (archives / first_record['relative']).stat().st_mtime_ns
    run(args)
    assert (archives / first_record['relative']).stat().st_mtime_ns == untouched
    values = [json.loads(p.read_text()) for p in (state / 'records').glob('*.json')]
    assert {v['status'] for v in values} == {'kept', 'complete'}, values
    for value in values:
        actual = run([binary, 'hash', archives / value['relative'], '-T', 2]).stdout.splitlines()
        member = 'game.slp' if value['relative'] == 'game.slpz' else 'opaque.bin'
        expected = hashlib.sha1((source / member).read_bytes()).hexdigest() + '  ' + member
        assert actual == [expected]
    # Simulate a crash after rename, before the completion record/backup cleanup.
    complete = next(v for v in values if v['status'] == 'complete')
    rp = state / 'records' / (complete['key'] + '.json')
    complete['status'] = 'prepared'
    rp.write_text(json.dumps(complete))
    archive = archives / complete['relative']
    backup = archive.with_name('.' + archive.name + '.before-recompress')
    backup.write_bytes(originals[archive.stem])
    saved = archive.read_bytes()
    run(args)
    assert archive.read_bytes() == saved and not backup.exists()
    assert json.loads(rp.read_text())['status'] == 'complete'
    # An incorrect decoded hash must leave the source archive unchanged.
    broken = root / 'broken-binary'
    broken.write_text('#!/usr/bin/env python3\nimport subprocess,sys\n'
                      f'binary={str(binary)!r}\n'
                      "if sys.argv[1]=='hash': print('0'*40+'  game.slp')\n"
                      'else: sys.exit(subprocess.call([binary,*sys.argv[1:]]))\n')
    broken.chmod(0o755)
    badroot = root / 'failure'
    badroot.mkdir()
    (badroot / 'game.slpz').write_bytes(originals['game'])
    badargs = ['python3', script, '--root', badroot, '--binary', broken, '--extractor', legacy,
               '--state-dir', root / 'badstate', '--threads', 2, '--apply']
    result = subprocess.run([str(x) for x in badargs], capture_output=True, text=True)
    assert result.returncode == 1 and 'hash mismatch' in result.stdout, result
    assert (badroot / 'game.slpz').read_bytes() == originals['game']
    assert not list(badroot.glob('*.before-recompress'))
print('migration: exact contents, resume, post-rename recovery, larger-output retention, and hash-failure safety passed')
