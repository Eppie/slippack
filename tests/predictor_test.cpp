#define SLPZ_FUZZ 1
#include "../slpz.cpp"

static size_t checks = 0;
static void check(bool ok, const char* why) {
  if (!ok) { fprintf(stderr, "predictor test: %s\n", why); abort(); }
  ++checks;
}
static void word(uint8_t* p, uint32_t n) {
  p[0] = uint8_t(n >> 24); p[1] = uint8_t(n >> 16);
  p[2] = uint8_t(n >> 8); p[3] = uint8_t(n);
}
int main() {
  std::fesetround(FE_TONEAREST);
  std::mt19937 random(821251);
  // Establish that the low-bit phase table covers the entire cycle once.
  uint32_t state = 0;
  std::array<bool, 8192> seen{};
  for (unsigned k = 0; k < 8192; ++k) {
    check(!seen[state & 8191], "repeated low-bit state");
    seen[state & 8191] = true;
    check(lcg_tables().phase[state & 8191] == k, "LCG phase");
    state = lcg_next(state);
  }
  check((state & 8191) == 0, "LCG low-bit period");
  // Use the original scalar stepper as an independent oracle, including
  // the cap, states beyond the cap, and full-state aliases of low bits.
  for (unsigned trial = 0; trial < 128; ++trial) {
    uint32_t start = random(), scalar = start;
    for (unsigned k = 0; k <= 8192; ++k) {
      if (k <= kSeedCap) {
        check(lcg_advance(start, k) == scalar, "LCG jump");
        check(lcg_distance(start, scalar) == k, "LCG bounded distance");
        check(lcg_distance(start, scalar ^ (1u << 13)) == UINT32_MAX, "LCG alias accepted");
      } else check(lcg_distance(start, scalar) == UINT32_MAX, "LCG out-of-window distance");
      scalar = lcg_next(scalar);
    }
  }
  for (unsigned trial = 0; trial < 4096; ++trial) {
    uint32_t a = random(), b = random(), scalar = a, expected = UINT32_MAX;
    for (unsigned k = 0; k <= kSeedCap; ++k) {
      if (scalar == b) { expected = k; break; }
      scalar = lcg_next(scalar);
    }
    check(lcg_distance(a, b) == expected, "LCG random pair");
  }
  const uint32_t special[] = {0, 0x80000000, 0x7f800000, 0xff800000,
      0x7fc01234, 0xffc05678, 0x7f801234, 0x00000001, 0x7f7fffff, 0xffffffff};
  for (unsigned version : {10u, 13u, 14u}) {
    bool modern = version >= 13;
    g_v13_predictors = modern;
    g_v14_predictors = version >= 14;
    for (unsigned size : {28u, 29u, 51u, 59u, 71u, 72u, 76u, 84u, 100u}) {
      for (unsigned count : {0u, 1u, 2u, 17u, 257u}) {
        Buf original;
        original.resize(size_t(size) * count);
        for (auto& b : original) b = uint8_t(random());
        for (unsigned i = 0; i < count; ++i)
          for (unsigned offset : {9u, 13u, 17u, 21u, 25u, 52u, 56u, 60u, 64u, 68u})
            if (offset + 4 <= size && i % 2 == 0)
              word(original.data() + size_t(i) * size + offset, special[(i + offset) % std::size(special)]);
        auto records = original;
        motion_predict(records.data(), count, size, false);
        if (version == 14 && size >= 29 && size < 72) {
          // Independent oracle: extrapolate from three original positions,
          // rather than keeping the implementation's velocity/acceleration.
          float positions[2][3] = {};
          for (unsigned i = 0; i < count; ++i) {
            for (unsigned j = 0; j < 2; ++j) {
              auto& h = positions[j];
              volatile float v0 = h[0] - h[1], v1 = h[1] - h[2];
              volatile float acceleration = v0 - v1;
              volatile float velocity = v0 + acceleration;
              volatile float next = h[0] + velocity;
              float prediction = std::isfinite(next) ? next : 0.f;
              uint32_t bits;
              memcpy(&bits, &prediction, 4);
              size_t offset = size_t(i) * size + 9 + 4 * j;
              uint32_t actual = be32(original.data() + offset), delta = actual - bits;
              check(be32(records.data() + offset) ==
                        ((delta << 1) ^ (0u - (delta >> 31))), "quadratic scalar oracle");
              h[2] = h[1]; h[1] = h[0];
              memcpy(&h[0], &actual, 4);
            }
          }
        }
        if (size < (modern ? 29u : 72u)) check(records == original, "predictor eligibility");
        motion_predict(records.data(), count, size, true);
        check(records == original, "numeric predictor roundtrip");
        input_predict(records.data(), count, size, false);
        if (version == 14 && size >= 59) {
          uint32_t previous[4][256] = {};
          const unsigned offsets[] = {24, 28, 32, 36}, raw[] = {58, 63, 64, 65};
          for (unsigned i = 0; i < count; ++i)
            for (unsigned j = 0; j < 4; ++j) {
              if (raw[j] >= size) continue;
              const auto* source = original.data() + size_t(i) * size;
              uint32_t actual = be32(source + offsets[j]);
              auto& last = previous[j][source[raw[j]]];
              uint32_t delta = actual - last;
              check(be32(records.data() + size_t(i) * size + offsets[j]) ==
                        ((delta << 1) ^ (0u - (delta >> 31))), "numeric input oracle");
              last = actual;
            }
        }
        input_predict(records.data(), count, size, true);
        check(records == original, "input predictor roundtrip");
      }
    }
  }
  g_v13_predictors = true;
  g_v14_predictors = false;
  Buf old_records; old_records.resize(51 * 3);
  for (unsigned i = 0; i < 3; ++i) {
    word(old_records.data() + i * 51 + 9, 0x3f800000u + (i == 2));
    word(old_records.data() + i * 51 + 13, 0x80000000u);
  }
  auto original = old_records;
  motion_predict(old_records.data(), 3, 51, false);
  check(be32(old_records.data() + 51 + 9) == 0, "old-format position repeat");
  check(be32(old_records.data() + 2 * 51 + 9) == 2, "one-ULP signed residual");
  check(be32(old_records.data() + 51 + 13) == 0, "negative-zero repeat");
  motion_predict(old_records.data(), 3, 51, true);
  check(old_records == original, "old-format fallback reconstruction");
  printf("predictors: %zu checks passed\n", checks);
}
