"""Immutable dataset roles for round3 qualification, separate from older protocols."""
import hashlib
import json

COHORTS = {
    'outside521': ('development-outside521-round3-inventory.json', 'development-outside-original521.txt', 1408),
    'development': ('development-round3-inventory.json', 'development-round3.txt', 1929),
    'holdout': ('holdout-round3-inventory.json', 'holdout-round3.txt', 512),
}


def check_cohorts(root, public, plan):
    assert set(plan['datasets']) == set(COHORTS)
    sets = {}
    for name, (inventory, manifest, count) in COHORTS.items():
        cohort = plan['datasets'][name]
        assert cohort['inventory'] == inventory
        assert cohort['manifest'] == f'build/goal-2026-11-12/{manifest}'
        assert cohort['files'] == count
        inventory_path = public / inventory
        manifest_path = root / cohort['manifest']
        assert hashlib.sha256(inventory_path.read_bytes()).hexdigest() == cohort['inventory_sha256']
        assert hashlib.sha256(manifest_path.read_bytes()).hexdigest() == cohort['manifest_sha256']
        inputs = json.loads(inventory_path.read_text())['inputs']
        sets[name] = {row['sha256'] for row in inputs}
        assert len(inputs) == len(sets[name]) == count
        paths = manifest_path.read_text().splitlines()
        assert len(paths) == len(set(paths)) == count
        reports = cohort['reports']
        assert reports == [f"{plan['candidate']}-{name}-round3-{count}-launch{i}.json" for i in (1, 2, 3)]
    original = {
        row['sha256'] for row in json.loads((public / 'development-round2-inventory.json').read_text())['inputs']
        if row['source'] == 'original-521'
    }
    assert len(original) == 521 and original <= sets['development']
    assert sets['outside521'] == sets['development'] - original
    assert not sets['holdout'] & sets['development']
    assert plan['holdout_inventory_sha256'] == plan['datasets']['holdout']['inventory_sha256']
    assert plan['development_launches_total'] == 3 and plan['repetitions_per_launch'] == 9
    assert plan['workers'] == [1, 8] and plan['levels'] == [3]
