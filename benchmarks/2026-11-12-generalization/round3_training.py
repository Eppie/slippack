"""Exact compiler-training references; evaluation cohorts are never training."""
import hashlib
import json


def training_reference(root, public, scheme):
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if scheme == 'original64':
        reference = root / 'build/goal-2026-11-12/candidate6/level3.json'
        frozen = json.loads((public / 'candidate6-build-info.json').read_text())
        assert digest(reference) == frozen['sha256'][str(reference.relative_to(root))]
        count = 64
        description = 'Same64 compiler-training inputs as frozen candidate6; no round3 heldout inputs'
    elif scheme == 'balanced400':
        reference = public / 'balanced-round3-training-plan.json'
        count = 400
        description = '400 development inputs:16 content-hash-selected files from each of25 source collections; no round3 heldout inputs'
    else:
        raise AssertionError('Unsupported compiler-training scheme')
    rows = json.loads(reference.read_text())['inputs']
    expected = [{key: row[key] for key in ('bytes', 'sha256')} for row in rows]
    hashes = {row['sha256'] for row in expected}
    assert len(expected) == len(hashes) == count
    development = {row['sha256'] for row in json.loads((public / 'development-round3-inventory.json').read_text())['inputs']}
    holdout = {row['sha256'] for row in json.loads((public / 'holdout-round3-inventory.json').read_text())['inputs']}
    assert hashes <= development and not hashes & holdout
    return expected, reference, description
