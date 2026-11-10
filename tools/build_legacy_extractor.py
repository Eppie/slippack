#!/usr/bin/env python3
"""Build the original v9 reader with extraction and hashing in one pass."""
from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=Path('build/legacy-extract'))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
source = subprocess.check_output(['git', 'show', '37345ca:slpz.cpp'], cwd=repo, text=True)
changes = {
    'mode == "x" || mode == "t" || mode == "hash"':
        'mode == "x" || mode == "xhash" || mode == "t" || mode == "hash"',
    '(mode == "x" && dir.empty())': '((mode == "x" || mode == "xhash") && dir.empty())',
    'mode != "x", mode == "hash"':
        'mode != "x" && mode != "xhash", mode == "hash" || mode == "xhash"',
}
for old, new in changes.items():
    assert source.count(old) == 1, old
    source = source.replace(old, new)
output = args.output.resolve()
output.parent.mkdir(parents=True, exist_ok=True)
cpp = output.with_suffix('.cpp')
cpp.write_text(source)
subprocess.run(['clang++', '-std=c++20', '-O3', '-I/opt/homebrew/include', str(cpp),
                '-o', str(output), '-L/opt/homebrew/lib', '-lzstd', '-llzma',
                '-lbrotlienc', '-lbrotlidec'], check=True)
print(output)
