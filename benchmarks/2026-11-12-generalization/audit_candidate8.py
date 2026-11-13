#!/usr/bin/env python3
"""Audit the completed, sleep-guarded candidate8 qualification against the goal."""
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess

from awake_protocol import check_cohorts, check_guard_reports
from reserve_round3 import family


PUBLIC = Path(__file__).resolve().parent
ROOT = PUBLIC.parent.parent
WORK = ROOT / 'build/goal-2026-11-12'
RUN = 'candidate8-awake1'
evidence = {}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(path):
    evidence[str(path.relative_to(ROOT))] = digest(path)
    return json.loads(path.read_text())


def verify(mapping, base=ROOT):
    for name, expected in mapping.items():
        path = base / name
        assert digest(path) == expected, str(path)
        evidence[str(path.relative_to(ROOT))] = expected


def main():
    target = PUBLIC / 'candidate8-completion-audit.json'
    assert not target.exists(), 'Preserve completed audit evidence'
    plan_path = PUBLIC / f'{RUN}-replication-protocol.json'
    plan = read(plan_path)
    check_cohorts(ROOT, PUBLIC, plan)
    info = read(PUBLIC / 'candidate8-build-info.json')
    assert digest(PUBLIC / 'candidate8-build-info.json') == plan['candidate_build_info_sha256']
    verify(info['sha256'])
    verify(plan['artifact_sha256'])
    incident = read(PUBLIC / plan['environmental_incident'])
    verify(incident['sha256'])
    baseline = read(WORK / 'baseline/provenance.json')
    assert baseline['commit'] == plan['baseline_commit'] == info['baseline_commit'] == '3d39721ef0e696d7a7d23c86c4805b37b45685e8'
    verify(baseline['sha256'], WORK / 'baseline')
    for name in list(baseline['sha256']) + ['profiles/level3.json', 'profiles/level3.proftext']:
        if name.endswith(('.cpp', '.h', '.json', '.proftext')) or name == 'Makefile':
            committed = subprocess.check_output(['git', 'show', baseline['commit'] + ':' + name], cwd=ROOT)
            assert hashlib.sha256(committed).hexdigest() == digest(WORK / 'baseline' / name), name

    validation = read(PUBLIC / 'candidate8-validation.json')
    assert validation['completed'] and all(row['exit_code'] == 0 for row in validation['checks'])
    assert digest(PUBLIC / 'candidate8-validation.json') == info['validation_sha256']
    verify(validation['artifact_sha256'])
    expected_log_text = ['custom codec: 7329 checks passed', 'Verified 19133569 model and CDF checks.',
                         'record codec: 111209 checks passed', 'predictors: 2463583 checks passed',
                         'archive API: 92 checks passed', 'archive integration:', 'extraction:']
    for mode in ('normal', 'scalar', 'sanitizers', 'profile-release'):
        text = (WORK / f'candidate8-{mode}.log').read_text()
        assert all(token in text for token in expected_log_text), mode
    assert '-fsanitize=address,undefined' in (WORK / 'candidate8-sanitizers.log').read_text()
    guard_validation = read(PUBLIC / 'awake-validation.json')
    assert guard_validation['completed'] and all(row['exit_code'] == 0 for row in guard_validation['checks'])
    verify(guard_validation['artifact_sha256'])

    pooled = read(PUBLIC / 'candidate8-pooled-check.json')
    assert pooled['completed'] and pooled['files'] == 1929 and pooled['all_pooled_archives_identical_to_parent']
    assert pooled['reference_candidate'] == 'candidate7'
    assert {(r['variant'], r['level']) for r in pooled['rows']} == {(v, l) for v in ('baseline', 'candidate') for l in (1, 3, 7, 9)}
    assert all(r['reconstructed_member_hashes_match'] for r in pooled['rows'])
    for level in (1, 3, 7, 9):
        assert len({r['archive_sha256'] for r in pooled['rows'] if r['level'] == level}) == 1
    fingerprints = read(PUBLIC / 'candidate8-fingerprint-audit.json')
    assert fingerprints['completed'] and fingerprints['files'] == 1929
    assert fingerprints['all_roundtrips_match'] and fingerprints['all_packets_identical_to_candidate7']
    assert fingerprints['library_sha256'] == digest(WORK / 'candidate8/library.a')
    assert pooled['binary_sha256']['candidate'] == digest(WORK / 'candidate8/slippack')
    assert digest(ROOT / 'slippack') == digest(ROOT / 'build/release/slippack') == digest(WORK / 'candidate8/slippack')
    assert digest(ROOT / 'build/release/libslippack.a') == digest(WORK / 'candidate8/library.a')
    evidence['slippack'] = digest(ROOT / 'slippack')

    inventories = {name: read(PUBLIC / data['inventory']) for name, data in plan['datasets'].items()}
    sets = {name: {x['sha256'] for x in inventory['inputs']} for name, inventory in inventories.items()}
    profile = read(ROOT / 'profiles/level3.json')
    assert profile['profile_sha256'] == digest(ROOT / 'profiles/level3.proftext')
    assert profile['inputs'] == read(WORK / 'candidate6/level3.json')['inputs']
    assert profile['inputs'] == read(WORK / 'baseline/profiles/level3.json')['inputs']
    assert len(profile['inputs']) == len({x['sha256'] for x in profile['inputs']}) == 64
    training = {x['sha256'] for x in profile['inputs']}
    assert training <= sets['development'] and not training & sets['holdout']
    reservation = read(PUBLIC / 'holdout-round3-protocol.json')
    verify(reservation['artifact_sha256'])
    dev_families = {family(x['source']) for x in inventories['development']['inputs']}
    hold_families = {family(x['source']) for x in inventories['holdout']['inputs']}
    assert len(hold_families) == 16 and not dev_families & hold_families
    assert sorted(dev_families) == reservation['excluded_families']
    counts = Counter(x['source'] for x in inventories['holdout']['inputs'])
    assert len(counts) == 16 and set(counts.values()) == {32}
    assert set(counts) == set(reservation['sources'])
    assert datetime.fromisoformat(reservation['reserved_utc']) < datetime.fromisoformat(info['freeze_utc'])
    overlap = read(PUBLIC / 'round3-gameplay-overlap-audit.json')
    assert overlap['files'] == 2441 and overlap['usable_event_streams'] == 2438
    assert overlap['cross_split_duplicate_groups'] == overlap['holdout_internal_duplicate_groups'] == 0
    assert not overlap['duplicate_groups']
    for split in ('development', 'holdout'):
        assert {r['sha256'] for r in overlap['inputs'] if r['split'] == split} == sets[split]

    actual_inputs = {}
    packet_by_hash = {'baseline': {}, 'candidate': {}}
    summaries, statuses, measurements = {}, {}, {}
    all_guards = []
    original_report = None
    for dataset, count in [('outside521', 1408), ('development', 1929), ('holdout', 512)]:
        data = plan['datasets'][dataset]
        s = read(PUBLIC / f'{RUN}-{dataset}-replication-summary.json')
        status = read(PUBLIC / f'{RUN}-{dataset}-replication-status.json')
        summaries[dataset], statuses[dataset] = s, status
        assert status['completed'] and status['completed_launches'] == [1, 2, 3]
        assert not status.get('aborted') and s['completed'] and s['qualifies_on_pooled_means']
        assert s['run_id'] == status['run_id'] == RUN and s['candidate'] == status['candidate'] == 'candidate8'
        assert s['protocol_sha256'] == status['protocol_sha256'] == digest(plan_path)
        assert datetime.fromisoformat(plan['declared_utc']) < datetime.fromisoformat(status['started_utc'])
        assert s['files'] == count and s['launches'] == 3 and s['repetitions_per_launch'] == 9
        assert list(s['report_sha256']) == data['reports']
        assert s['guard_report_sha256'] == check_guard_reports(ROOT, PUBLIC, plan, dataset)
        verify(s['report_sha256'], PUBLIC)
        verify(s['guard_report_sha256'], PUBLIC)
        reports = [read(PUBLIC / name) for name in data['reports']]
        guards = [read(PUBLIC / name) for name in data['guard_reports']]
        all_guards += guards
        paths = [Path(line) for line in (ROOT / data['manifest']).read_text().splitlines()]
        assert len(paths) == count
        for path, expected in zip(paths, reports[0]['inputs'], strict=True):
            key = str(path.resolve())
            if key not in actual_inputs:
                actual_inputs[key] = dict(sha256=digest(path), bytes=path.stat().st_size)
            assert actual_inputs[key]['sha256'] == expected['sha256']
            assert actual_inputs[key]['bytes'] == expected['bytes']
        for r in reports:
            assert r['completed'] and r['repeats'] == 9 and r['baseline_commit'] == baseline['commit']
            assert len(r['inputs']) == count and {x['sha256'] for x in r['inputs']} == sets[dataset]
            assert r['raw_bytes'] == sum(x['bytes'] for x in r['inputs']) == inventories[dataset]['raw_bytes']
            assert not r['power']['changes'] and r['power']['initial'] == r['power']['final'] == {'mode': 2, 'source': 'AC Power'}
            assert r['binary_sha256'] == {'baseline': digest(WORK / 'baseline/runner'), 'candidate': digest(WORK / 'candidate8/runner')}
            original_report = original_report or r
            assert r['source_sha256'] == original_report['source_sha256']
            verify(r['source_sha256'])
            assert set(r['rows']) == {f'{v}-l3-t{t}' for v in ('baseline', 'candidate') for t in (1, 8)}
            for v in ('baseline', 'candidate'):
                for t in (1, 8):
                    row = r['rows'][f'{v}-l3-t{t}']
                    assert len(row['encode_seconds']) == len(row['decode_seconds']) == 9
                    assert row['bytes'] == sum(row['file_bytes'])
                    for source, size, sha in zip(r['inputs'], row['file_bytes'], row['packed_sha1'], strict=True):
                        key = source['sha256']
                        assert packet_by_hash[v].setdefault(key, (size, sha)) == (size, sha)
        results = {}
        for t in (1, 8):
            results[str(t)] = {}
            for metric in ('encode', 'decode'):
                before, after = [[x for r in reports for x in r['rows'][f'{v}-l3-t{t}'][metric + '_seconds']]
                                 for v in ('baseline', 'candidate')]
                assert len(before) == len(after) == 27
                assert all(math.isfinite(x) and x > 0 for x in before + after)
                bm, cm = statistics.mean(before), statistics.mean(after)
                gain, reduction = 100 * (bm / cm - 1), 100 * (1 - cm / bm)
                expected = s['timings'][str(t)][metric]
                for key, value in [('baseline_seconds', bm), ('candidate_seconds', cm),
                                   ('throughput_gain_percent', gain), ('time_reduction_percent', reduction)]:
                    assert math.isclose(value, expected[key], abs_tol=1e-10), (dataset, t, metric, key)
                assert gain >= (10 if metric == 'encode' else 0)
                if metric == 'encode':
                    assert reduction >= 10  # Observed result also meets a 10% elapsed-time interpretation.
                results[str(t)][metric] = dict(throughput_gain_percent=gain, time_reduction_percent=reduction,
                                               launch_gains_percent=expected['launch_throughput_gains_percent'],
                                               descriptive_timing_interval=expected['descriptive_hierarchical_95_percent_interval'])
        b, c = [reports[0]['rows'][f'{v}-l3-t1']['file_bytes'] for v in ('baseline', 'candidate')]
        assert sum(c) <= sum(b)
        assert sum(b) == s['sizes']['baseline_bytes'] and sum(c) == s['sizes']['candidate_bytes']
        assert math.isclose(100 * (sum(c) / sum(b) - 1), s['sizes']['aggregate_change_percent'], abs_tol=1e-10)
        sources = {x['sha256']: x['source'] for x in inventories[dataset]['inputs']}
        assert len(s['source_sizes']) == {'outside521': 22, 'development': 25, 'holdout': 16}[dataset]
        for source, stats in s['source_sizes'].items():
            indexes = [i for i, x in enumerate(reports[0]['inputs']) if sources[x['sha256']] == source]
            assert len(indexes) == stats['files']
            assert sum(b[i] for i in indexes) == stats['baseline_bytes']
            assert sum(c[i] for i in indexes) == stats['candidate_bytes']
        measurements[dataset] = dict(files=count, timings=results, sizes=s['sizes'])

    assert len(actual_inputs) == 2441
    assert datetime.fromisoformat(info['freeze_utc']) < datetime.fromisoformat(plan['declared_utc'])
    for later, priors in [('development', ['outside521']), ('holdout', ['outside521', 'development'])]:
        for prior in priors:
            assert statuses[later]['prerequisite_summary_sha256'][prior] == digest(PUBLIC / f'{RUN}-{prior}-replication-summary.json')
            assert datetime.fromisoformat(statuses[prior]['finished_utc']) <= datetime.fromisoformat(statuses[later]['started_utc'])
    start = read(PUBLIC / f'{RUN}-holdout-start.json')
    assert start['started_utc'] == statuses['holdout']['started_utc']
    assert start['candidate_build_info_sha256'] == digest(PUBLIC / 'candidate8-build-info.json')
    preflight = read(PUBLIC / f'{RUN}-gate-preflight.json')
    assert preflight['completed'] and not preflight['holdout_encoded']
    assert all(c['expected_rejection'] and c['exit_code'] != 0 for c in preflight['checks'])
    evidence[str(Path(__file__).resolve().relative_to(ROOT))] = digest(Path(__file__))
    output = dict(completed=True, recorded_utc=datetime.now(timezone.utc).isoformat(),
                  candidate='candidate8', run_id=RUN, goal_option_satisfied=2,
                  conclusion='All three predeclared cohorts meet at least10% compression throughput gain and at least10% compression elapsed-time reduction at BOTH one/eight workers, with smaller aggregate output and faster average decoding. Independent heldout validation also passes.',
                  scope=dict(level=3, workers=[1, 8], machine=original_report['machine'], platform=original_report['platform'],
                             method=original_report['method'], power={'mode': 2, 'source': 'AC Power'}),
                  measurements=measurements,
                  safeguards=dict(all_9_launches_retained=True, measured_repetitions_per_cohort_worker=27,
                                  all_9_sleep_guards_valid=True, max_heartbeat_gap_seconds=max(g['max_observed_wall_gap_seconds'] for g in all_guards),
                                  unchanged_frozen_source_binaries_profile=True, installed_cli_matches_frozen=True,
                                  actual_input_files_rehashed=2441, profile_training_inputs=64, holdout_training_overlap=0,
                                  holdout_source_families=16, inputs_per_holdout_family=32, source_family_overlap=0,
                                  full_file_overlap=0, parsed_event_overlap=0, parsed_event_fallback_files=3,
                                  normal_scalar_sanitizer_release_tests_passed=True, four_level_pooled_roundtrips_passed=True,
                                  original_interrupted_attempt_preserved=True, no_old_samples_mixed=True),
                  limitations=['Means are empirical results on these datasets and this hardware, not a guarantee for every replay or machine.',
                               'Eight-worker independent launch gains range from7.01% to19.96%; the descriptive95% repeated-timing interval is7.30% to19.53%. All27 samples are retained. This is not an unseen-source confidence interval.',
                               'Compressed size is smaller in aggregate and on mean per-file percentage;105 of512 independent files and5 of16 source aggregates grow.',
                               'Family labels are heuristic; exact file/event hashes cannot exclude altered recordings. Source archives existed before this optimization effort.',
                               'Earlier independent candidate failures and the objectively sleep-interrupted attempt remain documented.'],
                  evidence_sha256=dict(sorted(evidence.items())))
    with target.open('x') as stream:
        json.dump(output, stream, indent=2)
        stream.write('\n')
    print('Completion audit PASS:', target.relative_to(ROOT), flush=True)
    print('All2441 input files rehashed; all9 guarded launches/all27 measurements per cohort-worker verified.', flush=True)


if __name__ == '__main__':
    main()
