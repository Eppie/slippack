# SLPZ v13 custom codec

The default writer still emits v9 (`--codec legacy`). Both `--codec tuned`
and `--codec custom` emit v13; header flag bit 0 selects the custom payload
coder. Tuned uses the existing compression libraries. The reader supports
v9, v10 (including the first custom backend), v11, v12, and v13. The CLI still
links the old libraries for compatibility; the standalone custom codec does
not link a compression library.

## Container and predictors

The v9 directory and framing are retained. Header flag bit 0 selects custom
payload compression in v10–v13. Codec ID 6 identifies tagged custom frames;
implicit frames use the backend from the header. Unknown versions or header
flag bits are rejected. Older readers reject v13 before reading its payload.
Container words are little-endian; source SLP numeric fields are big-endian.

The encoder omits stream packets for declared commands with no parsed events.
The original payload declaration table remains verbatim, so reconstruction
retains those declarations without storing empty selectors and plane models.
Streams containing actual zero-width events remain present. This is compatible
with existing readers and changes neither the container version nor framing.

The v10–v12 predictor representation below remains readable. Tuned and custom
share the same predictors in each version; v13's changes follow this section:

- Stream flag bit 4: post-frame velocity prediction for grouped 0x38 records
  at least 72 bytes long. Predict x/y from the previous position plus current
  self-induced and attack velocity; payload byte 46 selects ground/air x
  velocity. Both additions round separately to IEEE binary32, nearest/even.
  Non-finite predictions become +0. Store exact XOR residuals; each group
  starts with position zero.
- Stream flag bit 5: pre-frame input prediction for grouped 0x37 records at
  least 59 bytes long. XOR processed button bits with physical buttons, and
  the processed trigger with the physical trigger having the larger unsigned
  big-endian bit pattern. Available raw stick bytes index zero-initialized
  256-entry last-value tables predicting the corresponding float bits.

These follow pre/post cross-prediction during encoding and are reversed
before it during decoding. Unsupported record sizes skip these predictors.
Signed zero, infinities, and NaN payload bits are retained. Fast-math builds
are rejected and the CLI establishes nearest/even rounding for its workers.

## Numeric predictors (v13)

Stream flag bit 4 now applies to grouped post-frame (0x38) records at least
29 bytes long. V10–v12 retain the old 72-byte minimum and XOR residuals.
V13 chooses the position prediction from fields available in the record:

- At 72 bytes or more, use the same velocity-based binary32 prediction as
  before, with separately rounded additions and non-finite predictions zeroed.
- At 29–71 bytes, use the previous position's exact 32-bit pattern, initially
  zero. This fallback performs no floating-point arithmetic and retains NaN
  payloads and signed zeros.

For each x/y position, encode the unsigned modular difference between its
32-bit pattern and the predicted pattern. Zigzag the resulting signed bit
pattern: `(difference << 1) ^ (0u - (difference >> 31))`. Decode the difference
with `(code >> 1) ^ (0u - (code & 1))`, then add the predicted pattern modulo
2^32. This is a bijection for every 32-bit input, including exceptional floats.

The 4-byte fields at offsets 21, 25, 60, and 64 use the same difference/zigzag
mapping against the previous actual field value, initially zero, whenever the
whole field exists. Encoding computes positions before transforming those
fields; decoding restores the fields before computing the position prediction
because the velocity predictor reads offsets 60/64. State resets per group.

Input prediction is unchanged. The implementation specializes encoding and
decoding outside the record loop, including separate old/new position paths.
This removes per-record direction branches without changing old semantics.

## RNG acceleration (all readable versions)

RNG framing, group ordering, the 4096-step search cap, and raw-seed exceptions
are unchanged. A table maps the low 13 bits of each generator state to its
phase in the 8192-state low-bit cycle. Phase subtraction identifies at most
one candidate in the allowed window; the encoder verifies the full 32-bit
state using a precomputed affine jump before accepting it. The decoder uses
the same affine jump instead of walking every intermediate RNG state.
Ordinary step counts exceeding 4096 are rejected; the existing exception code
4097 still carries a raw state. Tests check the full low-bit cycle, every step
through and beyond the cap for random seeds, and unrelated/aliased states
against the original scalar stepper.

## Field dictionaries (v12)

Tuned and custom now sort each dictionary by the original big-endian field
byte sequence and remap its indexes. This improves both the value metadata
and temporal index deltas. It is an encoder policy: the dictionary explicitly
records the value assigned to every index.

Each record group begins with `u8 field_count`. The old representation is
unchanged: each field has `u8 width, u16 offset, u8 value_count_minus_one`, then
`value_count * width` raw value bytes. Width is 2 or 4; there are at most 255
non-overlapping fields and at most 256 values per field.

For compressed dictionaries, bit 7 of the first field's width byte is set.
Bits 4–6 hold a transform ID (1–4); the low four bits hold its width. Remaining
field width bytes are ordinary widths. All field descriptors appear first,
followed by **one tagged frame** (`u8 codec, u32 packed_size, u32 original_size,
payload`) containing all dictionary value bytes. Original size must exactly
match the sum derived from the field descriptors before allocating/decoding.
It is bounded by 255 × 256 × 4 bytes. Unknown transforms, invalid widths,
overlapping/out-of-bounds fields, invalid indexes, and size mismatches fail.

Dictionary values are shuffled into byte planes, separately for each field:

1. Plain shuffled bytes.
2. XOR each byte with the preceding byte of its plane, initially zero.
3. Subtract the preceding byte of its plane modulo 256, initially zero.
4. Interpret each original field as an unsigned big-endian integer, subtract
   the preceding field value modulo its width (initially zero), then shuffle
   the resulting big-endian difference bytes into planes.

Prediction resets at every field and, for modes 2/3, every byte plane. The
encoder selects a transform by estimated entropy plus model cost, compresses
it at level `min(requested_level, 3)`, and uses the frame only if its entire
serialized size is smaller than the raw dictionary values. Otherwise it
writes the original raw representation. Both backends use identical sorting,
transform selection, and fallback rules. Custom uses its own tagged payload
codec; tuned compares zstd and fast LZMA as for other small side streams.

Dictionary expansion specializes the 2-/4-byte widths and uses NEON register
lookups for alphabets up to 64 entries. Index validation precedes the lookup;
large alphabets and non-ARM builds use scalar gathers. These speed paths do
not affect the representation.

## Frames and planes

Frames remain `u32 packed_size, u32 original_size, packed_bytes`. Generic
custom frames start with a profile byte and a packet. Record layout 2 uses
one custom plane frame after the existing dictionaries and delta/XOR
selectors; these transforms are reversed after unpacking.

Profile IDs are generic 0, controller 1, float high 2, float low 3, RNG 4,
reserved RNG continuation 5, counter 6, and discrete/dictionary 7. Unknown
fields use generic. Profiles and context boundaries are serialized once per
plane, not per literal.

A v11 plane frame starts with varints `65536, plane_count, samples_per_plane`.
The sentinel distinguishes it from v10's `plane_count, samples_per_plane`.
Plane count is at most 65535 and dimensions must match the enclosing size.
Each plane has one descriptor:

- `1, varint packet_size, packet`: an explicit constant/sparse/run packet.
- `0, u8 profile`: a plane included in the remaining data.

If remaining data is nonempty, v11 stores a method byte: 0 for LZ of the
concatenated planes, or 3 for shared presence masks and exception values.
Methods 1 and 2 are reserved, not accepted. V10 has no method byte and uses LZ.

Very cheap planes use explicit packets. For substantial float-rich groups,
the encoder selects shared masks directly. Otherwise it compares the two
encoded candidates. These policies affect encoding only.

## Shared presence masks

For each remaining plane, store `u8 dominant_byte, u8 reference_distance`.
A reference distance of zero means no reference; other distances address an
already decoded plane. The encoder searches the previous eight masks.
One presence bit per sample indicates a value different from the dominant
byte. Bits are packed least significant first; each plane occupies
`ceil(samples_per_plane / 8)` bytes with zero padding.

The descriptors are followed by:

1. Mask method byte, varint packed mask size, packed mask data.
2. Value method byte and value payload.

Mask method 0 is a generic custom packet of the packed masks. Each mask is
XORed with its reference mask before coding and restored in plane order.
Method 2 is the two-bit presence coder below, using original masks. Method 1
is reserved. Value method 0 is LZ; method 1 is a direct entropy channel.
Exception values are concatenated in plane/sample order. Their contexts are
their plane profiles, derivable from the decoded masks. Reconstruction fills
each plane with its dominant byte and scatters the exceptions into set bits.

The presence coder processes four two-bit symbols per mask byte, reusing
neighbor-byte loads. This preserves the exact v11/v12 representation.
Decoding checks the all-zero/all-one pairs before the two mixed pairs; this
changes only the lookup order, not probabilities, state transitions, or bytes.

The ARM implementation packs/scatters eight values with NEON lookup tables.
The scalar implementation produces and accepts exactly the same format.
Temporary vector-load padding is in memory only, never part of the payload.

### Presence probability model

The payload starts with a split flag (0 or 1), then 128 or 256 varint
probabilities of a zero bit, each in 0..4096. Split 1 gives float-low planes
their own 128 contexts. The encoder includes model-header cost when choosing
the split. Four rANS states and renormalization bytes follow.

A bit context consists of the previous four presence bits in the current
plane and three reference-plane bits at sample offsets -1, 0, +1. Out-of-range
reference bits are zero. History starts at zero for each plane and updates
as `(history * 2 + bit) & 15`. The context index is
`history | (reference_bits << 4)`, plus 128 for float-low when split is set.
Reference bits are ordered from offset -1 in the least significant bit.

Two presence bits form one ANS symbol: `first_bit | (second_bit << 1)`.
For each starting history/reference window, the four symbol weights are the
products of their two binary frequencies, updating the context between bits.
Divide each weight by 4096, rounding down, retaining at least one for a
positive weight. Add any remaining frequency mass to the largest bin (first
on ties); if rounding up positive bins caused excess, remove it in symbol
order without zeroing a positive bin. Frequencies total 4096. Encoder and
decoder derive these small four-symbol tables identically, on demand.
An odd sample count has one extra zero decision. ANS state selection advances
continuously across planes, once per pair. Unused final mask bits must be zero.

## Generic packet modes

Original size is supplied by the enclosing frame or plane descriptor.

| Mode | Payload |
|---|---|
| 0 | Exact original bytes |
| 1 | One repeated byte |
| 2 | Dominant byte, exception count, gap channel, value channel |
| 3 | Run count, run-length channel, value channel |
| 4 | LZ payload |
| 5 | Direct entropy channel |

Sparse gaps are varints measured from the previous exception position plus
one, initially zero. Run lengths store length minus one as varints. Raw
storage remains an escape in generic packets and entropy channels.

## LZ payload

The representation is unchanged from v10:

1. Varint match count.
2. Channels of literal-run length codes, match-length codes, distance codes.
   There is one trailing literal-run length.
3. Varint extra-bit-stream size and its bytes. Extras are least-significant
   bit first, in token order: literal length, match length, distance.
4. A literal-prediction byte (0 or 1), then the literal channel.

Lengths below 16 are direct codes. Other lengths use
`16 + floor(log2(value)) - 4` with the lower `floor(log2(value))` bits as extras.
Match lengths encode length minus three. Distance codes 0..3 address a
move-to-front list initially `[1,2,3,4]`; others are
`4 + floor(log2(distance))` plus the lower bits. All matches update the list.
Predicted literals XOR the byte at the last match distance, initially one,
using zero when that distance precedes the output start.

The encoder now uses a four-byte chain plus a small three-byte lookup, a
1 MiB window, lazy parsing, and distance-sensitive match scoring. It rejects
candidates that cannot beat the current match before comparing full strings.
Long matches avoid redundant dictionary insertion; sustained misses permit
bounded skipping at fast levels. The CLI searches 2 candidates at levels 1–2,
4 at levels 3–5, 8 at levels 6–8, and 24 at level 9 or above. Literal prediction uses an entropy estimate
at fast levels and actual encoded-size trials at high levels. These are
encoder policies, not format requirements.

## Entropy channels

A channel is `varint packed_size, varint original_size, payload`:

- Mode 0: raw bytes.
- Mode 1: one static byte rANS model.
- Mode 2: eight semantic rANS models (v10 compatibility).
- Mode 4: static byte-context model. Mode 3 is reserved, not accepted.

A probability model stores a varint symbol count followed by
`(u8 symbol, varint frequency)` pairs. Frequencies sum to 4096. Unused models
have a dummy single-symbol distribution.

Mode 4 begins with a class count of 1 or 8. For each class it stores a leaf
count (1..16). Each leaf is an inclusive upper bound byte for the previous
symbol, then a probability model. Leaves cover 0..255 in increasing order.
LZ decoding builds per-literal semantic labels only when the serialized
channel actually uses them; raw and single-class channels do not need them.
The preceding decoded channel byte selects a leaf. History is zero initially
and when semantic class changes. With one class, semantic labels are ignored.

Tree construction aggregates the 256 possible history bytes into the 18
intervals defined by its candidate split boundaries. This produces the same
histograms and split decisions with less temporary storage and scanning.
Frequency normalization rounds scaled counts to the nearest integer before
correcting the total to 4096, retaining at least one count per used symbol.
The decoder continues to read the serialized frequencies directly.

For packets of at least 32 KiB with at least four controller planes, levels
below 9 skip the direct-LZ trial and use the shared-mask candidate. Level 9+
retains both trials for controller packets. This selection policy, like the
existing float-plane shortcut, is an encoder heuristic rather than a format
requirement; other distributions can favor the direct trial.

The encoder merges classes and splits context ranges only when estimated
entropy savings justify table costs. It uses these tables for literals and
LZ token channels; small channels use the simpler model.

Four byte-rANS states start and finish at `1 << 23`, using a 12-bit frequency
scale. Payloads store four little-endian state words followed by
renormalization bytes. Symbol `i` uses state `i mod 4`; encoding runs backwards.
Decoders validate table sums, dimensions, references, initial/terminal states,
padding, and full payload consumption. Framing adds no content checksum;
roundtrip verification uses external hashes.

## Validation and benchmarking

`make check` covers the original self-test, standalone custom packets/models,
shared masks, CDFs, truncation/mutation handling, and archive roundtrips and
extraction for all backends at levels 1/3/7/9. Predictor tests compare RNG tables with the scalar oracle
and verify old/new numeric predictors on arbitrary float bit patterns and
record-size boundaries. Record-codec tests additionally
force all dictionary transforms and cover vector boundaries, malformed
metadata, field resets, and arithmetic wraparound. Standalone custom tests and fuzzing
link no compression libraries. See `fuzz/README.md`.

`benchmarks/compare_custom.py` compares identical inputs, randomizes backend
order, warms once, records all timing samples, and verifies decoded file hashes.
Decode uses `slpz t`, including reconstruction and excluding file writes.
Results are hardware/level specific; superiority over slower, high-compression
LZMA presets is not implied by a level-3 comparison. Prior prototypes and negative results are
preserved in `benchmarks/2026-11-08-model-study/`.
