#!/usr/bin/env python3
"""Reserve source-family-separated inputs with the frozen baseline decoder only."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def family(source):
    name = source.removeprefix('slippi-gcs-')
    name = re.sub(r'^20\d\d-\d\d-\d\d-', '', name)
    if re.fullmatch(r'g[89]|gx(?:-?\d+)?(?:-streams)?|g[89]-streams', name):
        return 'genesis'
    for key in ('bair', 'summit', 'goml', 'tbh', 'full-bloom', 'tipped-off',
                'collision', 'supernova', 'combo-breaker', 'fight-pitt',
                'mainstage', 'genesis', 'bobc', 'riptide', 'htl', 'kill-roy',
                'lras', 'purdue-weeklies', 'abbey', 'gmus', 'lacs', 'gamercon',
                'nightclub', 'mango'):
        if key in name:
            return key
    return re.sub(r'-(?:20\d\d|\d+)(?:-|$).*', '', name)


def main():
    public = Path(__file__).resolve().parent
    root = public.parent.parent
    work = root / 'build/goal-2026-11-12'
    archives = root.parent / 'slpz_archives'
    development_path = public / 'development-round3-inventory.json'
    development = json.loads(development_path.read_text())
    seen = {r['sha256'] for r in development['inputs']}
    assert len(seen) == len(development['inputs']) == 1929
    used = {family(r['source']) for r in development['inputs']}
    seed = 'slippack-independent-round3-2026-11-13\n'
    ranked = sorted(
        (p.stem for p in archives.glob('*.slpc')
         if family(p.stem) not in used and not p.stem.startswith('slippi-gcs-event-')),
        key=lambda s: hashlib.sha256((seed + s).encode()).hexdigest(),
    )
    selected = []
    selected_families = set()
    for source in ranked:
        name = family(source)
        if name in selected_families:
            continue
        selected.append(source)
        selected_families.add(name)
        if len(selected) == 16:
            break
    assert len(selected) == 16
    source = public / 'sample_archive.cpp'
    baseline = work / 'baseline/library.a'
    helper = work / 'sample-round3-baseline'
    assert not helper.exists()
    subprocess.run(['c++', '-std=c++20', '-O3', '-pthread', str(source),
                    str(baseline), '-o', str(helper)], check=True)
    protocol_path = public / 'holdout-round3-protocol.json'
    protocol = dict(
        reserved_utc=datetime.now(timezone.utc).isoformat(),
        baseline_commit='3d39721ef0e696d7a7d23c86c4805b37b45685e8',
        sources=selected, source_families={s: family(s) for s in selected},
        selection='First16 distinct unused source families by SHA256(seed+source basename), excluding unidentified event-ID archives. Up to32 .slp members per source by smallest SHA1 of member path. No replacements for missing members or duplicates; no candidate measurement influences selection.',
        seed=seed, eligible_sources_in_hash_order=ranked,
        excluded_families=sorted(used),
        family_rules='Same conservative event-family rules as round2, additionally normalize leading ISO dates and group all gmus, lacs, gamercon, nightclub, and mango labels. Labels remain heuristic.',
        development_inventory_sha256=digest(development_path),
        artifact_sha256={str(p.relative_to(root)): digest(p)
                        for p in (Path(__file__), source, baseline, helper)},
        deduplication='Exclude full-file SHA256 matches to all1929 development files and within this selection. Run a frozen-baseline parsed-event-stream overlap audit before using this holdout.',
        use='Independent final validation only after candidate/source/profile freeze. No candidate encoding, timing, tuning, or profiling before final validation. Prior failed holdouts are explicitly retired.',
        development_gate='Require at least10% mean compression throughput improvement with BOTH one/eight workers, no aggregate size increase and no mean decode slowdown on BOTH full1929 development and the separate1408 development files outside original521. Fixed three-launch/nine-repeat qualification for a new frozen candidate, all samples retained; no additional unchanged-candidate launches to obtain a pass.',
        final_validation='Same frozen candidate, fixed three launches of nine interleaved repetitions on every eligible heldout input, one/eight workers. All launches retained without tuning between them. Same mean-based target, packed-size and decoding checks. Report per-source sizes and launch variation. Exact filenames/content hashes never select codec behavior.',
        limitations='Source archives were previously decoded/recompressed during migration. Family labels and exact event-stream hashes cannot rule out altered recordings. Earlier independent failures remain part of the evidence; this reservation does not erase them.',
    )
    with protocol_path.open('x') as stream:
        json.dump(protocol, stream, indent=2)
        stream.write('\n')
    print('Declared16 new source families:', ', '.join(selected), flush=True)
    rows, excluded, sources, paths = [], [], [], []
    for source_name in selected:
        archive = archives / (source_name + '.slpc')
        out = work / 'holdout-round3' / source_name
        assert not out.exists()
        before = digest(archive)
        subprocess.run([str(helper), str(archive), str(out), '32'], check=True)
        assert digest(archive) == before, source_name
        sources.append(dict(source=source_name, archive_bytes=archive.stat().st_size,
                            archive_sha256=before))
        for path in sorted(out.glob('*.slp')):
            sha = digest(path)
            row = dict(source=source_name, member_path_sha1=path.stem,
                       bytes=path.stat().st_size, sha256=sha)
            if sha in seen:
                excluded.append(row)
                continue
            seen.add(sha)
            rows.append(row)
            paths.append(str(path))
        print(source_name, 'baseline extraction and hashing complete', flush=True)
    assert digest(development_path) == protocol['development_inventory_sha256']
    for name, expected in protocol['artifact_sha256'].items():
        assert digest(root / name) == expected, name
    with (work / 'holdout-round3.txt').open('x') as stream:
        stream.write('\n'.join(paths) + '\n')
    with (public / 'holdout-round3-inventory.json').open('x') as stream:
        json.dump(dict(sources=sources, inputs=rows, excluded_duplicates=excluded,
                       raw_bytes=sum(r['bytes'] for r in rows),
                       protocol_sha256=digest(protocol_path)), stream, indent=2)
        stream.write('\n')
    print('Reserved', len(rows), 'files;', len(excluded), 'duplicates excluded;',
          'no candidate measurements.', flush=True)


if __name__ == '__main__':
    main()
