# SLPZ v11 custom codec

The default writer still emits v9 (`--codec legacy`). `--codec tuned` emits
v10 with the improved predictors and existing compression libraries.
`--codec custom` emits v11 and uses only the custom payload coder. The reader
supports v9, v10 (including the first custom backend), and v11. The CLI still
links the old libraries for compatibility; the standalone custom codec does
not link a compression library.

## Container and predictors

The v9 directory and framing are retained. Header flag bit 0 selects custom
payload compression in v10/v11. Codec ID 6 identifies tagged custom frames;
implicit frames use the backend from the header. Unknown versions or header
flag bits are rejected. Older readers reject v11 before reading its payload.
Container words are little-endian; source SLP numeric fields are big-endian.

The v10 predictors are unchanged, and are identical for tuned and custom:

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
bounded skipping at fast levels. Literal prediction uses an entropy estimate
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
The preceding decoded channel byte selects a leaf. History is zero initially
and when semantic class changes. With one class, semantic labels are ignored.

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
extraction for all backends at levels 1/3/9. Standalone custom tests and fuzzing
link no compression libraries. See `fuzz/README.md`.

`benchmarks/compare_custom.py` compares identical inputs, randomizes backend
order, warms once, records all timing samples, and verifies decoded file hashes.
Decode uses `slpz t`, including reconstruction and excluding file writes.
Results are hardware/level specific; superiority over slower, high-compression
LZMA presets is not implied by a level-3 comparison. Prior prototypes and negative results are
preserved in `benchmarks/2026-11-08-model-study/`.
