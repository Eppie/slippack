# Custom model and speed experiments

This continues commit `97809fd` (the first fully custom backend). The goal was
to beat tuned LZMA on size and both speeds while retaining identical upstream
parsing, dictionaries, and motion/input predictors. The final v11 implementation
is in the root headers; final measurements are in `../2026-11-08-v11/`.

`exploratory-results.json` preserves the intermediate measurements, binary
hashes, and input-list hashes. Many trials used only one timed repetition;
their timings are exploratory, not final performance claims. Sizes are exact
and every reported archive passed source-file hash verification.

## What worked

- Shared presence masks separate where a byte differs from its dominant value
  from the exception values themselves. Related float bytes often have the
  same exception locations even when their values differ.
- A plane can refer to one of eight preceding masks. Static bit contexts use
  recent frames and neighboring bits in that reference. Two binary decisions
  are combined into one four-symbol rANS operation with derived tables.
- Static previous-byte context trees add useful conditional modeling without
  an adaptive update per bit. Table cost controls class merging and splitting.
- A four-byte chain plus a short-match table, cost-sensitive selection, cheap
  candidate rejection, word comparisons, bounded skipping after repeated
  misses, and avoiding redundant insertion inside long matches reduce search
  work. Fast literal prediction uses an entropy estimate. Search depth stays
  at eight in the normal tier, two in the fast tier, and 24 in the high tier.
- Eliminating redundant coding trials and model construction matters. Large
  float-rich groups can select their shared-mask path directly.
- ARM NEON compacts/scatters eight exception values per step. A vectorized
  majority-zero check avoids a serial dominant-byte histogram on common
  planes. Scalar paths retain identical semantics and archive bytes.

## Approaches retained for another look

- **Adaptive models:** `adaptive_custom_codec.h` plus `adaptive_ans.h` is the
  early order-1/order-2 binary model with a forward probability trace and
  backwards rANS encoding. It reached 21,507,480 bytes on Summit, below tuned
  LZMA's 21,577,020. Mixed remained 18,686,755 versus 17,998,650. The forward
  updates and bitwise decoding were too expensive for the speed target.
  Better adaptation granularity or a different symbol coder could change that.
- **Four-bit presence symbols:** `four_bit_presence.h` retains that experiment.
  Larger derived tables cost more to build without a useful size improvement.
  The final two-bit model is in `mask_codec.h`; `binary_presence.h` and
  `two_bit_presence_draft.h` preserve earlier implementation fragments.
- **Reference selection by mutual information:** the tested estimate produced
  18,198,520 mixed bytes versus 18,089,324 for minimum XOR population. This
  does not rule out better conditional estimates or additional references.
- **Cross-plane XOR of exception values:** an order-0 entropy selector worsened
  mixed output from 17,902,407 to 17,942,041 bytes, including new header bytes.
  It can disrupt matches despite improving marginal entropy. A match-aware
  selector or precise float-field associations may be worth revisiting.
- **Distance deltas:** `distance-delta.patch` preserves the experiment against
  its then-current header. It saved only 669 bytes on mixed while adding a
  second distance-channel trial. This does not rule out more useful distance
  prediction, token models, or optimal parsing.
- **Shallower match search:** depth 1/2 and field-specific depth cuts reduced
  search work but often erased the mixed-set size advantage. Optimizing the
  eight-candidate search preserved the stronger ratio.

The 279 additional Summit files were initially held out, then used to identify
an encode-speed regression and validate general SIMD optimizations. They are
additional validation data, not a still-unseen test set. None of these models
uses a filename, tournament ID, replay identity, or pretrained corpus table.
