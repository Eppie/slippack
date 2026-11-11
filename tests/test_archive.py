#!/usr/bin/env python3
"""End-to-end format, predictor, fallback, and exact extraction checks."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
binary = repo / 'slippack'
golden = repo / 'tests/fixtures/v1-custom.slpc'

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


def check_group_partition(work):
    # Uneven, interleaved item groups exercise stable partition boundaries.
    # 255 groups is the format limit; the 256th must fall back to one stream.
    for groups in (1, 2, 17, 255, 256):
        raw = bytearray([0x35, 4, 0x3B, 0, 10])
        for frame in range(19):
            for group in reversed(range(groups)):
                if frame == 0 or (frame + group) % 3:
                    raw += bytes([0x3B]) + struct.pack('>IHI', frame, group, frame * 65537 + group)
        original = b'{U\x03raw[$U#l' + struct.pack('>I', len(raw)) + raw
        source = work / f'groups-{groups}.slp'
        source.write_bytes(original)
        packed = []
        for threads in (1, 8):
            archive = work / f'groups-{groups}-{threads}.slpc'
            run([binary, 'c', '-l', 3, '-T', threads, '-o', archive, source])
            destination = work / f'groups-{groups}-{threads}'
            run([binary, 'x', '-T', threads, archive, '-d', destination])
            assert (destination / source.name).read_bytes() == original
            packed.append(archive.read_bytes())
        assert packed[0] == packed[1], 'group task scheduling changed compressed output'


def verify(archive, work, expected, fixtures, label):
    assert archive.read_bytes()[:6] == b'SLPC\x01\x01'
    result = run([binary, 'hash', archive, '-T', '2'], text=True)
    assert sorted(result.stdout.splitlines()) == expected, label
    output = work / label
    run([binary, 'x', archive, '-d', output, '-T', '2'])
    assert all((output / name).read_bytes() == data for name, data in fixtures.items()), label


def main():
    fixtures = make_fixtures()
    expected = sorted(hashlib.sha1(data).hexdigest() + '  ' + name for name, data in fixtures.items())
    with tempfile.TemporaryDirectory(prefix='slippack-integration-') as temporary:
        work = Path(temporary)
        for name, data in fixtures.items():
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (work / 'list.txt').write_text('\n'.join(fixtures) + '\n')
        verify(golden, work, expected, fixtures, 'golden-v1')
        for level in (1, 3, 7, 9):
            archive = work / f'custom-{level}.slpc'
            run([binary, 'c', '-l', level, '-T', 2, '-o', archive, '@list.txt'], cwd=work)
            assert archive.read_bytes()[:6] == b'SLPC\x01\x01', 'new archives must use v1'
            verify(archive, work, expected, fixtures, f'out-{level}')
        declaration = work / 'declaration.slpc'
        run([binary, 'c', '-o', declaration, 'declared-only.slp'], cwd=work)
        assert declaration.stat().st_size < 512, 'unused declarations expanded into empty planes'
        check_group_partition(work)
        # Reject unsupported versions/flags before creating extracted files.
        original = golden.read_bytes()
        for version, flags in ([(v, f) for v in (0, *range(2, 17)) for f in (0, 1)]
                               + [(v, f) for v in (1,) for f in (0, 2, 3, 255)]):
            rejected = work / f'rejected-{version}-{flags}.slpc'
            rejected.write_bytes(original[:4] + bytes([version, flags]) + original[6:])
            output = work / f'rejected-{version}-{flags}'
            result = subprocess.run([str(binary), 'x', str(rejected), '-d', str(output)],
                                    capture_output=True, text=True)
            assert result.returncode == 1 and 'requires v1' in result.stderr
            assert not output.exists()
    print(f'archive integration: frozen v1 fixture, 4 levels, {len(fixtures)} fixtures, '
          'group partition boundaries, exact hashes/extraction, and unsupported-format rejection passed')


if __name__ == '__main__':
    main()
