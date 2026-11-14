# Built-in Gecko dictionary

`gecko-v1.json` records every training replay hash and every retained code-entry
hash, offset, and length. The dictionary is the concatenation of those entries
in SHA-256 order. The generated root header is compiled into the library, so
normal builds and decoding need no training files or external dictionary file.

ID 1 is frozen. Do not replace its bytes, reorder entries, or reuse its ID.
Adding a dictionary means assigning a new ID and retaining the old data in the
decoder. The generator refuses to overwrite an existing, different header.

The sample consists of the first eight replay hashes per eligible development
source. Source families whose SHA-256 first big-endian word modulo four equals
two were excluded before measurement. Only code entries present in at least two
selected replays were retained. This produced 596 entries / 136,904 bytes from
136 selected replays. The random comparison replay was not selected.

Reproduce from the original, hash-verified development inventory:

```sh
python3 tools/build_gecko_dictionary.py \
  --manifest build/goal-2026-11-12/development-round3.txt \
  --inventory benchmarks/2026-11-12-generalization/development-round3-inventory.json \
  --header gecko_dictionary_data.h \
  --provenance dictionaries/gecko-v1.json
```

The training extractor follows the Slippi [message splitter specification](https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md)
and the [Gecko entry lengths used by slippi-js](https://github.com/project-slippi/slippi-js/blob/master/src/common/utils/slpReader.ts).
It excludes incomplete entries and never repairs stale replay headers. Runtime
compression matches bytes directly and preserves all transport bytes, including
unused packet padding.

Excluded-source results test dictionary reuse across source families. These
files were already part of codec development, so they are not an independent
holdout for the entire compressor. See the [validation report](../benchmarks/2026-11-13-gecko-dictionary/README.md).
