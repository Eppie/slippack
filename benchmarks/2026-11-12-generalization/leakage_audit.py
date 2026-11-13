#!/usr/bin/env python3
"""Check frozen input/profile inventories for exact training/holdout overlap."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--development', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--holdout', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    reports = {key: json.loads(getattr(args, key).read_text())
               for key in ('development', 'profile', 'holdout')}
    hashes = {key: [r['sha256'] for r in report['inputs']] for key, report in reports.items()}
    development, training, holdout = map(set, (hashes['development'], hashes['profile'], hashes['holdout']))
    assert len(development) == len(hashes['development'])
    assert len(holdout) == len(hashes['holdout'])
    assert training <= development
    assert not (development & holdout)
    assert not (training & holdout)
    result = dict(development_files=len(development), profile_training_files=len(training),
                  holdout_files=len(holdout), development_holdout_duplicates=0,
                  training_holdout_duplicates=0, training_is_subset_of_development=True,
                  checked_inventory_sha256={key: hashlib.sha256(getattr(args, key).read_bytes()).hexdigest()
                                            for key in reports},
                  limitation='Exact SHA-256 deduplication does not identify altered recordings of the same game.')
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
