#!/usr/bin/env python3
"""Resumable, verified v9-to-custom migration. Defaults to an inventory dry run.

--apply extracts with the original reader, encodes with the new custom codec,
compares every decoded file hash, and atomically replaces only smaller archives.
A second invocation monitors the active process instead of duplicating work.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import stat
import struct
import subprocess
import threading
import time

GIB = 1 << 30


def atomic_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fingerprint(path):
    st = path.stat()
    return [st.st_size, st.st_mtime_ns, st.st_ino]


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def archive_index(path):
    """Read framing and the directory without decompressing any payload."""
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        at = 0
        def take(n):
            nonlocal at
            if n < 0 or at + n > len(data):
                raise ValueError(f'truncated archive: {path}')
            start = at
            at += n
            return start
        def number(fmt):
            return struct.unpack_from(fmt, data, take(struct.calcsize(fmt)))[0]
        def frame():
            n = number('<I')
            number('<I')
            take(n)
        if data[:4] != b'SLPZ':
            raise ValueError(f'bad archive magic: {path}')
        take(4)
        version, flags = number('<B'), number('<B')
        if version not in range(9, 16) or flags & ~1 or (version == 9 and flags):
            raise ValueError(f'unsupported archive: {path}')
        for _ in range(number('<I')):
            number('<B')  # command
            width = number('<I')
            number('<I')  # record count
            transformed = False
            for field in range(number('<B')):
                size = number('<B')
                if field == 0 and version != 9 and size & 128:
                    transformed = True
                    size &= 15
                number('<H')
                count = number('<B') + 1
                if size not in (2, 4) or width < size:
                    raise ValueError('invalid dictionary')
                if not transformed:
                    take(size * count)
                width -= size - 1 + (version >= 15 and count == 1)
            if transformed:
                number('<B')
                frame()
            take(width)
            layout = number('<B')
            if layout == 0:
                number('<B')
                frame()
            elif layout == 1:
                frame()
                number('<B')
                frame()
            elif layout == 2 and flags & 1:
                frame()
            else:
                raise ValueError('invalid layout')
        files = []
        unique = set()
        for _ in range(number('<I')):
            length = number('<I')
            offset = take(length)
            name = os.fsdecode(data[offset:offset + length])
            p = PurePosixPath(name)
            if (not p.parts or p.is_absolute() or '..' in p.parts or
                    any(c in name for c in '\n\r\x00')):
                raise ValueError(f'unsafe/unrepresentable member name: {name!r}')
            # Extraction on the local case-insensitive filesystem must be lossless.
            key = str(p).casefold()
            if key in unique:
                raise ValueError(f'colliding member names: {name!r}')
            unique.add(key)
            raw = number('<Q')
            packed = number('<Q')
            take(packed)
            files.append((name, raw))
        if at != len(data):
            raise ValueError(f'trailing archive data: {path}')
        return version, flags, files


def hashes(path):
    result = {}
    with path.open(encoding='utf-8', errors='surrogateescape') as stream:
        for line in stream:
            h, name = line.rstrip('\n').split('  ', 1)
            if len(h) != 40 or any(c not in '0123456789abcdef' for c in h) or name in result:
                raise ValueError(f'invalid hash manifest: {path}')
            result[name] = h
    return result


def eta_text(seconds):
    if seconds is None:
        return 'calculating'
    seconds = max(0, int(seconds))
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    return f'{hours:02d}:{minutes:02d}:{seconds:02d}'


def show_status(s):
    elapsed = s.get('active_seconds', 0)
    rate = s.get('raw_done_bytes', 0) / elapsed if elapsed else 0
    remaining = max(0, s.get('raw_total_bytes', 0) - s.get('raw_done_bytes', 0))
    eta = remaining / rate if rate else None
    if s.get('status') == 'complete':
        eta = 0
    print(f"{datetime.now():%H:%M:%S} | files {s.get('files_done', 0):,} processed / "
          f"{max(0, s.get('files_total', 0)-s.get('files_done', 0)):,} remaining | "
          f"avg {rate / 1e6:,.1f} MB/s | saved {s.get('saved_bytes', 0) / 1e6:,.1f} MB | "
          f"ETA {eta_text(eta)} | archives {s.get('archives_done', 0)}/{s.get('archives_total', 0)} | "
          f"{s.get('stage', s.get('status', 'starting'))} {s.get('current', '')}", flush=True)


def command(args, log, output=None, cwd=None):
    with log.open('ab') as err:
        out = output.open('wb') if output else err
        try:
            child = subprocess.Popen([str(a) for a in args], cwd=cwd, stdout=out, stderr=err)
            try:
                code = child.wait()
            except BaseException:
                child.send_signal(signal.SIGINT)
                try:
                    child.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                raise
            if code:
                raise RuntimeError(f'command exited {code}; see {log}')
        finally:
            if output:
                out.close()


def refresh_sidecars(root, state_dir):
    path = root / 'replays-sorted/chunks.json'
    if not path.exists():
        return
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or not isinstance(data.get('parts'), list):
        return
    changed = False
    for part in data['parts']:
        archive = path.parent / part['part']
        size = archive.stat().st_size
        if part.get('slpz_bytes') != size:
            part['slpz_bytes'] = size
            changed = True
    if changed:
        backup = state_dir / 'replays-sorted-chunks.before.json'
        if not backup.exists():
            shutil.copy2(path, backup)
        atomic_json(path, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--extractor', type=Path, required=True)
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--threads', type=int, default=16)
    parser.add_argument('--level', type=int, default=3)
    parser.add_argument('--status-interval', type=float, default=5)
    parser.add_argument('--limit', type=int, default=0, help='process at most N pending archives')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    root, state_dir = args.root.resolve(), args.state_dir.resolve()
    binary, extractor = args.binary.resolve(), args.extractor.resolve()
    if args.threads < 1 or args.status_interval <= 0:
        parser.error('threads and status interval must be positive')
    state_dir.mkdir(parents=True, exist_ok=True)
    status_path = state_dir / 'status.json'
    lock = (state_dir / 'migration.lock').open('a+')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('Migration already running; monitoring its status (Ctrl-C stops this monitor).', flush=True)
        while True:
            if status_path.exists():
                show_status(json.loads(status_path.read_text()))
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return 0
            except BlockingIOError:
                time.sleep(args.status_interval)
    records = state_dir / 'records'
    records.mkdir(exist_ok=True)
    work = state_dir / 'work'
    work.mkdir(exist_ok=True)
    identity = {'root': str(root), 'binary_sha256': digest(binary),
                'extractor_sha256': digest(extractor), 'level': args.level}
    identity_path = state_dir / 'identity.json'
    if identity_path.exists() and json.loads(identity_path.read_text()) != identity:
        raise RuntimeError('root, binary, extractor, or level changed; use the original migration settings')
    atomic_json(identity_path, identity)
    cached = {}
    inventory_path = state_dir / 'inventory.json'
    if inventory_path.exists():
        cached = {e['relative']: e for e in json.loads(inventory_path.read_text())}
    entries = []
    paths = sorted(root.rglob('*.slpz'))
    last_notice = time.monotonic()
    for index, path in enumerate(paths, 1):
        if path.is_symlink():
            raise RuntimeError(f'symlink archive requires separate handling: {path}')
        relative = path.relative_to(root).as_posix()
        current = fingerprint(path)
        previous = cached.get(relative)
        if previous and previous.get('fingerprint') == current:
            entries.append(previous)
        else:
            version, flags, files = archive_index(path)
            key = hashlib.sha256(os.fsencode(relative)).hexdigest()[:24]
            entries.append({'relative': relative, 'key': key, 'bytes': current[0],
                            'raw_bytes': sum(n for _, n in files), 'files': len(files),
                            'version': version, 'flags': flags, 'fingerprint': current})
        if time.monotonic() - last_notice >= args.status_interval:
            print(f'{datetime.now():%H:%M:%S} | inventory {index}/{len(paths)} archives | '
                  f"{sum(e['files'] for e in entries):,} member files found", flush=True)
            last_notice = time.monotonic()
    atomic_json(state_dir / 'inventory.json', entries)
    print(f"Inventory: {len(entries):,} archives, {sum(e['files'] for e in entries):,} files, "
          f"{sum(e['raw_bytes'] for e in entries)/1e9:,.1f} GB raw, "
          f"{sum(e['bytes'] for e in entries)/1e9:,.1f} GB archived.", flush=True)
    if not args.apply:
        print('Dry run only. Add --apply to migrate.')
        return 0
    old_status = json.loads(status_path.read_text()) if status_path.exists() else {}
    clock_base = old_status.get('active_seconds', 0)
    started = time.monotonic()
    status = {'status': 'running', 'stage': 'preflight', 'current': '',
              'archives_total': len(entries), 'files_total': sum(e['files'] for e in entries),
              'raw_total_bytes': sum(e['raw_bytes'] for e in entries),
              'archives_done': 0, 'files_done': 0, 'raw_done_bytes': 0, 'saved_bytes': 0,
              'active_seconds': clock_base}
    status_lock = threading.Lock()
    stopped = threading.Event()
    def update(**fields):
        with status_lock:
            status.update(fields)
    def snapshot(display=True):
        with status_lock:
            status['active_seconds'] = clock_base + time.monotonic() - started
            copy = dict(status)
        atomic_json(status_path, copy)
        if display:
            show_status(copy)
    def ticker():
        while not stopped.wait(args.status_interval):
            snapshot()
    def completed(record):
        with status_lock:
            status['archives_done'] += 1
            status['files_done'] += record['files']
            status['raw_done_bytes'] += record['raw_bytes']
            status['saved_bytes'] += record['old_bytes'] - record['new_bytes']
    thread = threading.Thread(target=ticker, daemon=True)
    thread.start()
    attempted = 0
    failures = []
    try:
        # Small archives first provide an early throughput estimate and free space.
        for entry in sorted(entries, key=lambda e: (e['raw_bytes'], e['relative'])):
            relative, key = entry['relative'], entry['key']
            path = root / relative
            rp = records / (key + '.json')
            folder = work / key
            candidate = path.with_name('.' + path.name + '.recompressing')
            backup = path.with_name('.' + path.name + '.before-recompress')
            record = json.loads(rp.read_text()) if rp.exists() else None
            if record and record['relative'] != relative:
                raise RuntimeError('state key collision')
            if record and record['status'] in ('complete', 'kept', 'prepared'):
                # Finish an interrupted atomic replacement using its verified digest.
                current = fingerprint(path)
                if record['status'] == 'prepared':
                    current_digest = digest(path)
                    if current_digest == record['new_sha256']:
                        record['status'] = 'complete'
                        record['fingerprint'] = current
                        atomic_json(rp, record)
                    elif current_digest != record['old_sha256']:
                        raise RuntimeError(f'archive changed during interrupted migration: {relative}')
                if record['status'] in ('complete', 'kept'):
                    if current != record['fingerprint'] and digest(path) != record['new_sha256']:
                        raise RuntimeError(f'completed archive changed: {relative}')
                    if backup.exists():
                        if digest(backup) != record['old_sha256']:
                            raise RuntimeError(f'backup changed: {backup}')
                        backup.unlink()
                    if candidate.exists():
                        candidate.unlink()
                    if folder.exists():
                        shutil.rmtree(folder)
                    completed(record)
                    continue
            if args.limit and attempted >= args.limit:
                continue
            attempted += 1
            update(current=relative, stage='checking')
            begin = time.monotonic()
            try:
                if entry['version'] != 9:
                    raise RuntimeError('pending archive is not v9; refusing an untracked replacement')
                need = entry['raw_bytes'] + entry['bytes'] + entry['files'] * 8192 + 10 * GIB
                if shutil.disk_usage(work).free < need:
                    raise RuntimeError(f'insufficient scratch space: needs {need/1e9:.1f} GB')
                if backup.exists():
                    if not record or digest(backup) != record.get('old_sha256'):
                        raise RuntimeError('unrecognized backup; preserving it')
                    backup.unlink()  # original is still in place after an interrupted prepare
                if folder.exists():
                    shutil.rmtree(folder)
                folder.mkdir()
                if candidate.exists():
                    if not record:
                        raise RuntimeError('unrecognized candidate; preserving it')
                    candidate.unlink()
                original_stat = path.stat()
                before = fingerprint(path)
                old_sha = digest(path)
                record = {**entry, 'status': 'working', 'old_bytes': entry['bytes'],
                          'old_sha256': old_sha, 'started': datetime.now().isoformat()}
                atomic_json(rp, record)
                source = folder / 'source'
                reference = records / (key + '.source.sha1')
                restored = records / (key + '.custom.sha1')
                log = records / (key + '.log')
                update(stage='extracting + hashing')
                command([extractor, 'xhash', path, '-d', source, '-T', args.threads], log, reference)
                expected = hashes(reference)
                _, _, files = archive_index(path)
                if set(expected) != {name for name, _ in files}:
                    raise RuntimeError('original hash manifest names do not match archive directory')
                for name, size in files:
                    if (source / name).stat().st_size != size:
                        raise RuntimeError(f'extracted size mismatch: {name}')
                filelist = folder / 'files.txt'
                filelist.write_text(''.join(name + '\n' for name, _ in files), errors='surrogateescape')
                update(stage='compressing')
                command([binary, 'c', '--codec', 'custom', '-l', args.level, '-T', args.threads,
                         '-o', candidate, '@' + str(filelist)], log, cwd=source)
                update(stage='verifying every file')
                command([binary, 'hash', candidate, '-T', args.threads], log, restored)
                if hashes(restored) != expected:
                    raise RuntimeError('decoded content/name hash mismatch; original preserved')
                version, flags, result_files = archive_index(candidate)
                if version != 15 or flags != 1 or Counter(result_files) != Counter(files):
                    raise RuntimeError('new archive directory mismatch')
                if fingerprint(path) != before:
                    raise RuntimeError('original archive changed while processing; refusing replacement')
                new_bytes = candidate.stat().st_size
                record.update(new_bytes=min(new_bytes, entry['bytes']),
                              candidate_bytes=new_bytes, elapsed_seconds=time.monotonic() - begin,
                              file_hash_manifest_sha256=digest(reference))
                if new_bytes >= entry['bytes']:
                    candidate.unlink()
                    record.update(status='kept', new_sha256=old_sha, fingerprint=before)
                    atomic_json(rp, record)
                else:
                    update(stage='atomic replacement')
                    record.update(new_sha256=digest(candidate), status='prepared')
                    os.chmod(candidate, stat.S_IMODE(original_stat.st_mode))
                    os.utime(candidate, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
                    with candidate.open('rb') as stream:
                        os.fsync(stream.fileno())
                    os.link(path, backup)
                    sync_directory(path.parent)
                    atomic_json(rp, record)
                    os.replace(candidate, path)
                    sync_directory(path.parent)
                    record.update(status='complete', fingerprint=fingerprint(path))
                    atomic_json(rp, record)
                    backup.unlink()
                    sync_directory(path.parent)
                shutil.rmtree(folder)
                completed(record)
            except Exception as error:
                failures.append({'archive': relative, 'error': str(error)})
                if record and record.get('status') not in ('prepared', 'complete'):
                    record.update(status='failed', error=str(error))
                    atomic_json(rp, record)
                print(f'ERROR {relative}: {error}', flush=True)
                # Failed source/candidate bytes are left for inspection; the original stays intact.
        refresh_sidecars(root, state_dir)
        atomic_json(state_dir / 'failures.json', failures)
        done = status['archives_done'] == len(entries)
        update(status='complete' if done else 'incomplete', stage='complete' if done else 'paused/incomplete', current='')
        return 0 if done or (args.limit and not failures) else 1
    except KeyboardInterrupt:
        update(status='interrupted', stage='interrupted; rerun the same command to resume')
        return 130
    except BaseException:
        update(status='failed', stage='failed; originals and verified replacements preserved')
        raise
    finally:
        stopped.set()
        thread.join()
        snapshot()


if __name__ == '__main__':
    raise SystemExit(main())
