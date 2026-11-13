#!/usr/bin/env python3
"""Declare all three round3 qualification cohorts before any canonical timing."""
import argparse
from datetime import datetime,timezone
import hashlib,json,re
from pathlib import Path
from round3_cohorts import COHORTS,check_cohorts
from round3_training import training_reference

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',required=True)
    parser.add_argument('--reason',required=True)
    args=parser.parse_args()
    assert re.fullmatch(r'candidate[0-9]+',args.candidate)
    public=Path(__file__).resolve().parent;root=public.parent.parent;work=root/'build/goal-2026-11-12'
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    target=public/f'{args.candidate}-replication-protocol.json'
    assert not target.exists()
    assert not (public/'holdout-round3-retirement.json').exists()
    info_path=public/f'{args.candidate}-build-info.json';info=json.loads(info_path.read_text())
    expected,training_plan,_=training_reference(root,public,info['training_scheme'])
    assert info['training_inputs']==len(expected) and not info['heldout_evaluated']
    assert info['training_reference']==str(training_plan.relative_to(root))
    assert info['training_plan_sha256']==digest(training_plan)
    profile=json.loads((work/args.candidate/'level3.json').read_text())
    assert profile['inputs']==expected
    pooled_path=public/f'{args.candidate}-pooled-check.json'
    pooled=json.loads(pooled_path.read_text())
    assert pooled['completed'] and pooled['files']==1929
    assert pooled['binary_sha256']['candidate']==digest(work/args.candidate/'slippack')
    assert {(row['variant'],row['level']) for row in pooled['rows']}=={(variant,level) for variant in ('baseline','candidate') for level in (1,3,7,9)}
    assert all(row['reconstructed_member_hashes_match'] for row in pooled['rows'])
    leakage_path=public/f'{args.candidate}-leakage-audit.json'
    leakage=json.loads(leakage_path.read_text())
    assert leakage['development_files']==1929 and leakage['holdout_files']==512
    assert leakage['profile_training_files']==len(expected) and leakage['training_is_subset_of_development']
    assert leakage['development_holdout_duplicates']==leakage['training_holdout_duplicates']==0
    assert leakage['checked_inventory_sha256']==dict(development=digest(public/'development-round3-inventory.json'),profile=digest(work/args.candidate/'level3.json'),holdout=digest(public/'holdout-round3-inventory.json'))
    fingerprint_path=public/f'{args.candidate}-fingerprint-audit.json'
    fingerprint=json.loads(fingerprint_path.read_text())
    assert fingerprint['completed'] and fingerprint['files']==1929 and fingerprint['all_roundtrips_match']
    assert fingerprint['library_sha256']==digest(work/args.candidate/'library.a')
    assert fingerprint['manifest_sha256']==digest(work/'development-round3.txt')
    assert info['reserved_holdout_inventory']=='holdout-round3-inventory.json'
    assert info['reserved_holdout_inventory_sha256']==digest(public/'holdout-round3-inventory.json')
    for name,sha in info['sha256'].items():assert digest(root/name)==sha,name
    plan=dict(declared_utc=datetime.now(timezone.utc).isoformat(),candidate=args.candidate,baseline_commit='3d39721ef0e696d7a7d23c86c4805b37b45685e8',reason=args.reason,status_at_declaration='No canonical timing for this frozen candidate has begun. New round3 holdout has never been candidate encoded, trained, profiled, or timed.',development_launches_total=3,repetitions_per_launch=9,workers=[1,8],levels=[3],datasets={},candidate_build_info_sha256=digest(info_path),holdout_inventory_sha256=digest(public/'holdout-round3-inventory.json'))
    artifacts=[public/name for name in ['declare_round3_protocol.py','round3_cohorts.py','round3_training.py','replicate_round3.py','summarize_replicates_round3.py','summarize_replicates.py','summarize.py','development-round2-inventory.json','holdout-round3-protocol.json','round3-gameplay-overlap-audit.json','candidate6-build-info.json']]
    artifacts += [training_plan,pooled_path,leakage_path,fingerprint_path]
    artifacts += [root/'benchmarks/2026-11-10-encode-speed/benchmark.py',root/'benchmarks/2026-11-10-single-file/cpp_runner.cpp']
    artifacts += [work/'baseline'/name for name in ['runner','library.a','slippack','runner-common.o']]
    for name,(inventory,manifest,count) in COHORTS.items():
        plan['datasets'][name]=dict(inventory=inventory,inventory_sha256=digest(public/inventory),manifest=f'build/goal-2026-11-12/{manifest}',manifest_sha256=digest(work/manifest),files=count,reports=[f'{args.candidate}-{name}-round3-{count}-launch{i}.json' for i in (1,2,3)])
        artifacts += [public/inventory,work/manifest]
        assert not any((public/report).exists() for report in plan['datasets'][name]['reports'])
        assert not (public/f'{args.candidate}-{name}-replication-status.json').exists()
        assert not (public/f'{args.candidate}-{name}-replication-summary.json').exists()
    plan['artifact_sha256']={str(p.relative_to(root)):digest(p) for p in sorted(set(artifacts))}
    plan['aggregation']='Retain all27 measured repetitions from exactly3 process launches per cohort. Ratio of pooled elapsed means; no dropped samples, favorable-launch selection, or fourth qualification launch.'
    plan['cohort_order']=['outside521','development','holdout']
    plan['acceptance']='Each cohort must independently show at least10% compression throughput gain at BOTH one and eight workers, no aggregate size growth and no average decoding slowdown. Any failed development gate returns to development without consuming the new holdout. Holdout must pass the identical rule.'
    plan['fixed_artifacts']='Same frozen candidate and original3d39721 baseline, original shared driver, level3, stable AC power, source/binary/profile/input fingerprints; no changes between qualification launches or cohorts.'
    plan['uncertainty']='Development cohorts overlap and have influenced tuning; neither is independent validation. Final512 are32 per16 new source families with full-file and parsed-gameplay duplicate audits. Bootstrap intervals describe repeated timing only, not unseen-source uncertainty.'
    check_cohorts(root,public,plan)
    with target.open('x') as output:output.write(json.dumps(plan,indent=2)+'\n')
    print('Declared immutable outside521 -> full development -> holdout protocol for',args.candidate)

if __name__=='__main__':main()
