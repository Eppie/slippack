# SLPZ v10 experimental custom codec

The default writer still emits v9. Version 10 adds reversible predictors and
an optional custom entropy backend. No v9 codec ID or parameter definition is
changed. Integer container words remain little-endian; source SLP numeric
fields remain big-endian.

## Container and compatibility

The v9 container directory and framing are retained. In v10, header flag bit
0 selects custom payload compression. With that bit clear, existing entropy
codecs are used (`--codec tuned`). Unknown versions or header flags are
rejected. Codec ID 6 identifies the custom backend in tagged frames. Implicit
frames, such as the arbitrary-input fallback, use the backend selected by the
container header. A v9 reader rejects v10 before decoding any frames.

Two additional stream flags are allowed in v10:

- Bit 4: post-frame velocity prediction, for grouped 0x38 records at least
  72 payload bytes long. Predict x/y from the preceding decoded position plus
  this record's self-induced and attack velocity; choose ground/air x velocity
  using payload byte 46. Store a bitwise XOR residual. Each group starts at
  position zero. The two additions round separately to IEEE binary32,
  round-to-nearest/ties-to-even. A non-finite predicted result becomes +0.
- Bit 5: pre-frame input prediction, for grouped 0x37 records at least 59 bytes
  long. XOR the processed button low half with physical buttons and the
  processed trigger with the physical trigger having the larger unsigned
  big-endian bit pattern. For each available raw stick axis, predict its float
  bits using a 256-entry last-value table indexed by the raw axis byte; tables
  start at zero. Store XOR residuals and update with original/decoded bits.

These operate after the existing pre/post cross-prediction during encoding.
Decode reverses them per group before undoing cross-prediction. Unsupported
record sizes skip the corresponding predictor. NaN payloads, signed zero, and
other exceptional input bits are retained by the residuals. Fast-math builds
are rejected; the CLI establishes round-to-nearest before starting workers.

## Frames and record planes

All frames retain `u32 packed_size, u32 original_size, packed_bytes`.
Generic custom frames begin with a profile byte followed by a custom packet.
Record groups use new packed layout 2: one custom plane frame following the
existing dictionary fields and per-plane delta/XOR selectors. The original
per-byte transforms are still reversed after unpacking.

Profiles distinguish generic bytes, controllers, float high/low bytes, RNG,
counters, and dictionary/discrete values. RNG continuation profile ID 5 is
reserved for future model selection. Literals are modeled by the profile of
their original plane. Metadata, unfamiliar commands, and fields without a
known layout use the generic model. Model selection includes serialized model
cost; a single model or raw bytes can win over semantic contexts.

A plane frame stores unsigned base-128 varints for plane count and samples
per plane, then one descriptor per plane:

- `1, varint packet_size, packet`: an explicit constant/sparse/repeat packet.
- `0, u8 profile`: a remaining plane, concatenated into the shared LZ input.

The remaining concatenated planes share a match dictionary and use their
recorded profile boundaries for literal probability models. If there are no
remaining bytes, no LZ payload follows. The decoder checks the plane dimensions
against the enclosing frame size.

## Custom packet modes

Original size is supplied by the enclosing frame or plane descriptor.

| Mode | Payload |
|---|---|
| 0 | Exact original bytes |
| 1 | One repeated byte |
| 2 | Dominant byte, exception count, encoded gap and exception-value channels |
| 3 | Run count, encoded run-length and value channels |
| 4 | LZ payload |

Sparse gaps are unsigned varints measured from the previous exception position
plus one, initially zero. Run lengths store length minus one as varints.
Every chosen candidate includes headers in its size comparison. Plane
specialization conservatively screens very sparse or run-heavy planes; other
planes remain together to retain cross-plane matches.

## LZ payload

The encoder uses a three-byte hash chain, a 1 MiB maximum distance, four recent
distances, a distance-cost estimate, and optional one-byte lazy matching. Match
finder depth and parsing policy are encoder choices and need no format flag.

An LZ payload contains:

1. Match count as a varint.
2. Entropy channels for literal-run length codes, match-length codes, and
   distance codes. There is one extra trailing literal-run length.
3. Varint extra-bit-stream size and the extra bits, packed least significant
   bit first. Extras follow token order: literal length, match length, distance.
4. A byte selecting raw or predicted literals, then the literal channel.

Lengths below 16 have direct codes. Other lengths use
`16 + floor(log2(value)) - 4` and store the lower `floor(log2(value))` bits.
Match lengths encode length minus three. Distance codes 0–3 address a move-to-
front list initially `[1,2,3,4]`. Other codes are `4 + floor(log2(distance))`
plus the lower bits. Every match updates the recent-distance list.

Predicted literals XOR the byte at the most recent match distance, using zero
before enough output exists. The initial distance is one. Raw and predicted
literal channels are compared by actual encoded size. Literal contexts are
derived from the token positions and plane profiles before literal decoding.

## Entropy channels

A channel is `varint packed_size, varint original_size, payload`:

- Mode 0: raw bytes.
- Mode 1: one static rANS model.
- Mode 2: eight semantic rANS models.

Each model stores a varint symbol count and `(u8 symbol, varint frequency)`
pairs. Frequencies total 4096. Unused contexts have a dummy single-symbol
model. Four independent byte-rANS states start and finish at `1 << 23`.
The channel stores four little-endian state words followed by renormalization
bytes. Symbol `i` uses state `i mod 4`; encoding traverses symbols backwards.
Decode checks frequency sums, initial/terminal states, bounds, and complete
payload consumption. The framing does not add a content checksum; use hashes
for external roundtrip verification.

## Validation and benchmarks

`make check` runs the original self-test, standalone custom codec tests, and
archive roundtrips/extraction for all three backends at levels 1, 3, and 9.
Fixtures include empty/garbage/truncated/in-progress files and floating-point
edge cases. `fuzz/custom_codec_fuzz.cpp` exercises bounded custom decoding and
encode/decode identities without linking any external codec library.

`benchmarks/compare_custom.py` tests identical file lists with legacy, tuned,
and custom backends. It randomizes backend order, warms once, records median
times, and compares each decoded file's SHA-1 against its source. Decode timing
uses `slpz t`, including reconstruction but excluding filesystem writes.
