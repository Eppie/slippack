#!/usr/bin/env python3
"""End-to-end format, predictor, fallback, and exact extraction checks."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
binary = repo / 'slpz'
golden = repo / 'tests/fixtures/v15-custom.slpz'

def replay(post_size=84, unused=False):
    sizes = {0x37: 66, 0x38: post_size, 0x3A: 12}
    if unused:
        sizes.update({0x3D: 65535, 0x40: 4096})
    raw = bytearray([0x35, 1 + 3 * len(sizes)])
    for cmd, size in sizes.items():
        raw += bytes([cmd]) + struct.pack('>H', size)
    seed = 12345
    previous = [bytes(84), bytes(84)]
    for frame in range(260):
        raw += bytes([0x3A]) + struct.pack('>iII', frame-123, seed, frame)
        for player in range(2):
            pre, post = bytearray(66), bytearray(84)
            struct.pack_into('>iBB', pre, 0, frame-123, player, 0)
            struct.pack_into('>iBB', post, 0, frame-123, player, 0)
            seed = (seed * 214013 + 2531011) & 0xffffffff
            struct.pack_into('>I', pre, 6, seed)
            pre[10:24] = previous[player][7:21]
            pre[59:63] = previous[player][21:25]
            for offset, raw_offset in [(24,58),(28,63),(32,64),(36,65)]:
                pre[raw_offset] = (frame+player) % 127
                struct.pack_into('>f', pre, offset, pre[raw_offset]/128)
            struct.pack_into('>H', pre, 48, frame % 8192)
            pre[46:48] = pre[48:50]
            for offset in (40,50,54):
                struct.pack_into('>f', pre, offset, (frame % 11)/10)
            struct.pack_into('>H', post, 7, frame % 24)
            for offset in (9,13): struct.pack_into('>f', post, offset, frame/4 + player)
            for offset in (52,56,68): struct.pack_into('>f', post, offset, 0.25)
            post[46] = frame % 2
            # Preserve infinities, signed zero, and distinct NaN payloads.
            if frame in (30,31,32,33):
                struct.pack_into('>I', post, 9, [0x7fc01234,0x7f800000,0x80000000,0xffc05678][frame-30])
            raw += bytes([0x37]) + pre + bytes([0x38]) + post[:post_size]
            previous[player] = bytes(post)
    return b'{U\x03raw[$U#l' + struct.pack('>I', len(raw)) + raw + b'metadata verbatim'

def make_fixtures():
    good = replay()
    # Large unused declarations are retained verbatim but require no empty
    # record packets. A zero-width declaration with events must remain active.
    def wrap(raw):
        return b'{U\x03raw[$U#l' + struct.pack('>I', len(raw)) + raw
    declared_only = wrap(bytes([0x35, 4, 0x3D, 255, 255]))
    command_only = wrap(bytes([0x35, 4, 0x41, 0, 0]) + bytes([0x41]) * 129)
    fixtures = {'valid.slp': good, 'empty.slp': b'', 'garbage.slp': bytes(range(256))*3,
                'truncated.slp': good[:-77], 'in-progress.slp': good[:11]+bytes(4)+good[15:],
                'tiny.slp': b'\x00', 'nested/another.slp': good, 'old-format.slp': replay(51),
                'short-post.slp': replay(28), 'unused-types.slp': replay(unused=True),
                'declared-only.slp': declared_only, 'command-only.slp': command_only}
    # Relative PPC branches become a constant destination under the transform.
    # Cover both a single opaque frame and multiple independently coded chunks.
    def ppc(words):
        return b''.join(struct.pack('>I', 0x48000000 | (((4096 - i) * 4) & 0x03fffffc))
                        for i in range(words))
    fixtures['ppc.slp'] = good + ppc(18000)
    fixtures['chunked.slp'] = good + ppc(280000)
    fixtures['opaque.slp'] = good + bytes(range(256)) * 300
    return fixtures


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], capture_output=True, check=True, **kwargs)


def verify(archive, work, expected, fixtures, label):
    assert archive.read_bytes()[:6] in (b'SLPZ\x01\x01', b'SLPZ\x0f\x01')
    result = run([binary, 'hash', archive, '-T', '2'], text=True)
    assert sorted(result.stdout.splitlines()) == expected, label
    output = work / label
    run([binary, 'x', archive, '-d', output, '-T', '2'])
    assert all((output / name).read_bytes() == data for name, data in fixtures.items()), label


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, help='also compare against a pre-cleanup v15 binary')
    parser.add_argument('--write-golden', action='store_true', help='generate the fixture with --reference')
    args = parser.parse_args()
    reference = args.reference.resolve() if args.reference else None
    if args.write_golden and reference is None:
        parser.error('--write-golden requires --reference')
    fixtures = make_fixtures()
    expected = sorted(hashlib.sha1(data).hexdigest() + '  ' + name for name, data in fixtures.items())
    with tempfile.TemporaryDirectory(prefix='slpz-integration-') as temporary:
        work = Path(temporary)
        for name, data in fixtures.items():
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (work / 'list.txt').write_text('\n'.join(fixtures) + '\n')
        if args.write_golden:
            golden.parent.mkdir(exist_ok=True)
            run([reference, 'c', '--codec', 'custom', '-l', 3, '-T', 2, '-o', golden, '@list.txt'], cwd=work)
        verify(golden, work, expected, fixtures, 'golden-v15')
        renamed = work / 'golden-v1.slpz'
        old_bytes = golden.read_bytes()
        assert old_bytes[:6] == b'SLPZ\x0f\x01'
        renamed.write_bytes(old_bytes[:4] + b'\x01' + old_bytes[5:])
        verify(renamed, work, expected, fixtures, 'golden-v1')
        for level in (1, 3, 7, 9):
            archive = work / f'custom-{level}.slpz'
            run([binary, 'c', '-l', level, '-T', 2, '-o', archive, '@list.txt'], cwd=work)
            assert archive.read_bytes()[:6] == b'SLPZ\x01\x01', 'new archives must use v1'
            verify(archive, work, expected, fixtures, f'out-{level}')
            if reference:
                baseline = work / f'reference-{level}.slpz'
                run([reference, 'c', '--codec', 'custom', '-l', level, '-T', 2,
                     '-o', baseline, '@list.txt'], cwd=work)
                current = archive.read_bytes()
                original_header = current[:4] + b'\x0f' + current[5:]
                assert original_header == baseline.read_bytes(), f'payload changed at level {level}'
                # The old reader understands the identical payload with its original header.
                compatible = work / f'compatible-{level}.slpz'
                compatible.write_bytes(original_header)
                result = run([reference, 'hash', compatible, '-T', 2], text=True)
                assert sorted(result.stdout.splitlines()) == expected
        declaration = work / 'declaration.slpz'
        run([binary, 'c', '--codec', 'custom', '-o', declaration, 'declared-only.slp'], cwd=work)
        assert declaration.stat().st_size < 512, 'unused declarations expanded into empty planes'
        # Reject unsupported versions/backends before creating extracted files.
        original = golden.read_bytes()
        for version, flags in ([(v, f) for v in (0, *range(2, 15), 16) for f in (0, 1)]
                               + [(v, f) for v in (1, 15) for f in (0, 2, 3, 255)]):
            rejected = work / f'rejected-{version}-{flags}.slpz'
            rejected.write_bytes(original[:4] + bytes([version, flags]) + original[6:])
            output = work / f'rejected-{version}-{flags}'
            result = subprocess.run([str(binary), 'x', str(rejected), '-d', str(output)],
                                    capture_output=True, text=True)
            assert result.returncode == 1 and 'requires v1 custom' in result.stderr
            assert not output.exists()
        for codec in ('legacy', 'tuned'):
            output = work / f'{codec}.slpz'
            result = subprocess.run([str(binary), 'c', '--codec', codec, '-o', str(output), 'valid.slp'],
                                    cwd=work, capture_output=True, text=True)
            assert result.returncode == 1 and 'only the custom codec' in result.stderr
            assert not output.exists()
    print(f'archive integration: v1/v15-header compatibility, 4 levels, {len(fixtures)} fixtures, '
          'exact hashes/extraction, and unsupported-format rejection passed')


if __name__ == '__main__':
    main()
