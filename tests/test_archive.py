#!/usr/bin/env python3
"""End-to-end format, predictor, fallback, and exact extraction checks."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

binary = Path(__file__).resolve().parents[1] / 'slpz'

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

with tempfile.TemporaryDirectory(prefix='slpz-integration-') as work:
    work = Path(work)
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
    for name, data in fixtures.items():
        target = work/name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    (work/'list.txt').write_text('\n'.join(fixtures)+'\n')
    expected = sorted(hashlib.sha1(data).hexdigest()+'  '+name for name,data in fixtures.items())
    for codec in ('legacy','tuned','custom'):
        for level in (1,3,7,9):
            archive = work/f'{codec}-{level}.slpz'
            subprocess.run([str(binary),'c','--codec',codec,'-l',str(level),'-T','2','-o',str(archive),'@list.txt'],cwd=work,capture_output=True,check=True)
            header = archive.read_bytes()[:6]
            assert header[:4] == b'SLPZ'
            assert header[4:] == (b'\x09\x00' if codec=='legacy' else b'\x0e\x01' if codec=='custom' else b'\x0e\x00')
            result = subprocess.run([str(binary),'hash',str(archive),'-T','2'],capture_output=True,text=True,check=True)
            assert sorted(result.stdout.splitlines()) == expected
            out = work/f'out-{codec}-{level}'
            subprocess.run([str(binary),'x',str(archive),'-d',str(out),'-T','2'],capture_output=True,check=True)
            assert all((out/name).read_bytes()==data for name,data in fixtures.items())
        archive = work/f'{codec}-declaration.slpz'
        subprocess.run([str(binary),'c','--codec',codec,'-l','3','-T','2','-o',str(archive),
                        'declared-only.slp'],cwd=work,capture_output=True,check=True)
        assert archive.stat().st_size < 512, 'unused declaration expanded into empty plane descriptors'
    print('archive integration: 12 codec/level combinations, 12 fixtures, hashes and extraction passed')
