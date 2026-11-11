# SLIPPACK v1 custom codec

The writer and reader use version 1 with header flags exactly 1. The rANS/LZ
codec is self-contained. Codec ID 6 identifies tagged frames; implicit frames
use the same compression. Other versions and unknown header flags are rejected.

Container words are little-endian; source Slippi numeric fields are big-endian.

## Container

```
u32 magic "SLPC", u8 version=1, u8 flags=1
u32 pool_count
  per pool: u8 command, u32 record_size, u32 record_count, record_packet
u32 file_count
  per file: u32 name_length, name bytes, u64 original_size, u64 blob_length, blob

blob = u8 mode, payload
  mode 0: custom_frame of the entire original file
  mode 1: original 15-byte Slippi header, original 0x35 declaration,
          packed_blob(metadata suffix), packed_blob(unparsed tail),
          tagged_frame(event order), u8 stream_count, stream packets
stream = u8 command, u32 record_size, u32 record_count, u8 flags,
         [tagged_frame(seed codes)],
         [tagged_frame(group selectors), u8 group_count,
          per group: u32 count, record_packet]
         or [record_packet] for ungrouped streams
         or [no payload] for pooled streams
```

Stream flag bits are 0 grouped, 1 seed folding, 2 pre/post cross-prediction,
3 pooled, 4 motion prediction, and 5 controller-input prediction. Pooled
streams refer to the matching command/record-size pool; offsets follow file
order. The encoder omits declared streams without events but keeps their
original declaration bytes. Actual zero-width events remain present.

`custom_frame` is `u32 packed_size, u32 original_size, packed_bytes`.
A tagged frame prefixes it with `u8 codec=6`. Generic packed bytes begin with
a profile byte; record plane frames use their own plane-packet structure.
Record packets contain dictionaries, one transform selector per output plane,
`u8 layout=2`, and one plane frame. Layouts 0 and 1 are unsupported.

Opaque metadata/tail blobs use these modes:

| Mode | Payload |
|---|---|
| 0 | One implicit custom frame |
| 2 | One tagged custom frame |
| 3 | `u8 codec=6, u8 PPC_transform, u32 chunk_count`, then custom frames |
| 4 | One tagged custom frame, then inverse PPC transform |

Mode 1 is unsupported. PPC transforms operate on complete 4-byte words and
reset word indexes for each chunk. The encoder currently uses 1 MiB chunks.

## Predictors

Pre/post cross-prediction runs before motion/input prediction on encoding and
is reversed after them on decoding. All state resets per player/follower group.
Float operations round separately to IEEE binary32 nearest/even. Nonfinite
predictions become positive zero; residuals preserve every original bit,
including signed zero and NaN payloads. Fast-math builds are rejected.

Stream flag bit 4 applies to grouped post-frame records (0x38) of at least
29 bytes. For records of at least 72 bytes, predict each position from the
previous position plus current self-induced and attack velocity; byte 46
selects the ground/air x velocity. Each addition rounds separately. For
29–71-byte records, use `position + (velocity + acceleration)`. After restoring
the actual position, update `nextVelocity = actualPosition - position`, then
`acceleration = nextVelocity - velocity`, velocity, and position, rounding
each subtraction separately. Initial state is positive zero.

Encode the unsigned modular difference between actual and predicted bit
patterns as `(difference << 1) ^ (0u - (difference >> 31))`. Invert it with
`(code >> 1) ^ (0u - (code & 1))`, adding the predicted bits modulo 2^32.
Encoding computes positions before transforming other post-frame fields;
decoding restores those fields before computing velocity-based positions.

Stream flag bit 5 applies to grouped pre-frame records (0x37) of at least
59 bytes. XOR processed buttons with physical buttons, and the processed
trigger with the physical trigger whose unsigned big-endian bits are larger.
Controller floats use the mappings and prediction tables below.

- Pre-frame controller floats at offsets 24, 28, 32, and 36 are first permuted:
  each exact binary32 representation of `q/80`, for integer `q` in [-80,80],
  is swapped with the unsigned bit pattern of `zigzag(q)`. Zero maps to itself.
  Other bit patterns pass through, including negative zero and NaN payloads.
  The explicit table in `controller_grid` defines the exact representations.
  Applying the same permutation again is the inverse, including for tiny
  subnormals whose bit patterns are the small codes.
- Controller prediction retains the per-axis 256-entry table keyed by raw
  offsets 58, 63, 64, and 65, and adds a 4,096-entry direct-mapped cache per
  available axis. The pair key is `raw_axis | (raw_other_axis << 8)` when both
  raw axes exist. Otherwise it is `raw_axis XOR ((mapped_other_float_bits &
  0x7fffffff) * 2246822519)`, with unsigned 32-bit multiplication. The slot is
  `(key * 2654435761) >> 20`. Entries store the 32-bit key and previous mapped
  actual value; all-ones entries are empty. A matching nonempty entry predicts;
  a miss falls back to the one-axis table. Both tables learn the actual value.
  Prediction and zigzag residual arithmetic operate on mapped bits; inverse
  mapping happens after restoring all four controller values in a record.
- Post-frame attack velocities at 60 and 64 predict `last + (last - older)`
  from actual float values, initially zero. Percent and shield at 21 and 25
  use unsigned-bit-pattern differences.
- The post-frame animation counter at 33 uses the action state at 7–8. States
  below 1,024 keep an entry value (initially zero) and positive step (initially
  one). On an action change, predict the saved entry value and update it with
  the actual counter. On the same action, predict `last_counter + step`; update
  the step to `actual - last_counter` only when the actual counter increased.
  States outside the table predict `last_counter + 1`. The previous action
  state and actual counter are updated for every record.
- Before computing the animation residual, swap each exact integer float in
  [-4096,4096] with `zigzag(integer)`, using the same reversible permutation
  rule as above. Map both actual and predicted values, take their unsigned
  modular difference, then zigzag it. The decoder restores mapped actual bits,
  inverts the permutation, and updates float predictor state.


## Constant dictionaries

A dictionary with one value has no index plane, transform selector, or
semantic profile. Its known value is filled directly during reconstruction.
Output width is `record_size - sum(field_width - 1) - single_value_field_count`.
The encoder tries complete known numeric fields before sliding 4-/2-byte
windows. These choices are encoded in the dictionary descriptors.

## RNG coding

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

## Field dictionaries

The encoder sorts each dictionary by the original big-endian field
byte sequence and remap its indexes. This improves both the value metadata
and temporal index deltas. It is an encoder policy: the dictionary explicitly
records the value assigned to every index.

The encoder skips candidate dictionaries with exactly
one nonconstant byte and at least 128 distinct values in that byte. Constant
planes already represent the fixed neighbors cheaply. Alphabet counts are
cached by byte offset and stop at the threshold. Constant dictionaries and
small one-byte alphabets avoid hashing every full record; dictionary values
and first-occurrence indexes are otherwise unchanged. This is an encoder
policy, not a new dictionary representation.

Byte-transform selection probes up to three 256-byte windows
(first, middle, last). Explicit plane
selectors carry every decision, so this also needs no new decoder semantics.

Each record group begins with `u8 field_count`. The raw representation uses `u8 width, u16 offset, u8 value_count_minus_one`, then
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
writes the original raw representation. Dictionary data uses the tagged custom payload codec.

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

A plane frame starts with varints `65536, plane_count, samples_per_plane`.
The decoder also accepts the original unextended plane-packet representation
without this sentinel.
Plane count is at most 65535 and dimensions must match the enclosing size.
Each plane has one descriptor:

- `1, varint packet_size, packet`: an explicit constant/sparse/run packet.
- `0, u8 profile`: a plane included in the remaining data.

If remaining data is nonempty, the packet stores a method byte: 0 for LZ of the
concatenated planes, or 3 for shared presence masks and exception values.
Methods 1 and 2 are reserved, not accepted. Unextended plane packets omit
the method byte and use LZ. These self-describing payload variants remain
valid inside the current format.

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
neighbor-byte loads. This preserves the two-bit presence representation.
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

### Four-bit presence coding (method 4)

Method 4 retains method 2's serialized split flag, 128/256 binary zero
probabilities at scale 4096, plane references, and four interleaved rANS states.
It combines four consecutive presence bits into one symbol, least-significant
bit first. Pad the final symbol of each plane with zero bits. The joint table
key contains the previous four bits (most recent in bit 0), six referenced-plane
bits from position `i-1` through `i+4`, and the optional FloatLow class.

For each of 16 symbols, multiply its four conditional binary frequencies
(updating history and referenced position after each bit). Convert this 48-bit
weight to a 16-bit CDF frequency by `(weight + 2^31) >> 32`, with minimum one
for nonzero weights and zero for impossible symbols. If frequencies undershoot
65536, add the deficit to the first largest-frequency symbol. If they overshoot,
remove excess in ascending symbol order without reducing a positive frequency
below one. Cumulative starts follow ascending symbol order.

The rANS lower bound remains `2^23`, but its probability scale is 65536 rather
than 4096. State selection advances for every nibble across all planes. Final
states must all equal the lower bound. Method 2 remains readable. The encoder
reconstructs history from source masks while walking backward instead of
storing a symbol trace. Lazy table construction and packed literal decode
lookups are implementation optimizations, with no additional wire fields.

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

The payload contains:

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
- Mode 2: eight semantic rANS models.
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

`make check` covers standalone custom packets/models, shared masks, CDFs,
truncation/mutation handling, and current-format archive roundtrips at levels 1/3/7/9.
A committed synthetic fixture from the pre-cleanup encoder checks existing
decoding under both version headers; unsupported archive versions and backends must fail before
extraction. Predictor tests compare the RNG tables, quadratic positions,
velocity-based positions, integer mapping, and extra field residuals with
independent scalar oracles on arbitrary float bit patterns and record sizes.
Record-codec tests force dictionary transforms and cover vector boundaries,
malformed metadata, field resets, and arithmetic wraparound.
No tests require external compression libraries. See `fuzz/README.md`.

Historical benchmark scripts compare the backends available at their recorded
commits. They warm inputs, retain timing samples, and verify decoded hashes.
Decode timings use `slippack t`, including reconstruction and excluding writes.
Results depend on hardware, level, and dataset. Earlier prototypes and negative
results are retained under `benchmarks/` and in Git history.
