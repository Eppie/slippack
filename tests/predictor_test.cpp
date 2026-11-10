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
// Preserve the original floating-point mapping as an independent oracle.
static uint32_t integer_grid_reference(uint32_t value) {
  if (value <= 8192) {
    int n = int(value >> 1) ^ -int(value & 1);
    float f = float(n);
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
  }
  uint32_t magnitude = value & 0x7fffffffu;
  if (magnitude > 0x45800000u) return value;
  float f;
  memcpy(&f, &magnitude, 4);
  unsigned n = unsigned(f);
  float g = float(n);
  uint32_t u;
  memcpy(&u, &g, 4);
  return u == magnitude && magnitude ? n * 2 - (value >> 31) : value;
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
  for (uint32_t value = 0; value < 100000; ++value) {
    uint32_t bits = value < 8193 ? value : random();
    check(integer_grid(bits) == integer_grid_reference(bits), "integer mapping oracle");
    check(integer_grid(integer_grid(bits)) == bits, "integer permutation");
    check(controller_grid(controller_grid(bits)) == bits, "controller permutation");
  }
  for (int i = -4100; i <= 4100; ++i) {
    float f = float(i);
    uint32_t bits;
    memcpy(&bits, &f, 4);
    for (uint32_t near : {bits - 1, bits, bits + 1})
      check(integer_grid(near) == integer_grid_reference(near), "integer boundary oracle");
  }
  const uint32_t special[] = {0, 0x80000000, 0x7f800000, 0xff800000,
      0x7fc01234, 0xffc05678, 0x7f801234, 0x00000001, 0x7f7fffff, 0xffffffff};
  for (unsigned version : {10u, 13u, 14u, 15u}) {
    bool modern = version >= 13;
    g_v13_predictors = modern;
    g_v14_predictors = version >= 14;
    g_v15_format = version >= 15;
    for (unsigned size :
         {28u, 29u, 51u, 59u, 63u, 64u, 65u, 66u, 67u, 68u, 71u, 72u, 76u, 84u, 100u}) {
      for (unsigned count : {0u, 1u, 2u, 17u, 257u}) {
        Buf original;
        original.resize(size_t(size) * count);
        for (auto& b : original) b = uint8_t(random());
        for (unsigned i = 0; i < count; ++i)
          for (unsigned offset : {9u, 13u, 17u, 21u, 25u, 33u, 52u, 56u, 60u, 64u, 68u})
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
        if (version == 15 && size >= 29) {
          for (unsigned offset : {21u, 25u, 60u, 64u}) {
            if (offset + 4 > size) continue;
            uint32_t previous = 0, older = 0;
            for (unsigned i = 0; i < count; ++i) {
              uint32_t prediction = previous;
              if (offset >= 60) {
                float x, y;
                memcpy(&x, &previous, 4);
                memcpy(&y, &older, 4);
                volatile float delta = x - y;
                volatile float next = x + delta;
                float safe = std::isfinite(next) ? next : 0.f;
                memcpy(&prediction, &safe, 4);
              }
              size_t at = size_t(i) * size + offset;
              uint32_t actual = be32(original.data() + at), delta = actual - prediction;
              check(be32(records.data() + at) == ((delta << 1) ^ (0u - (delta >> 31))),
                    "v15 field scalar oracle");
              older = previous;
              previous = actual;
            }
          }
        }
        if (version >= 13 && size >= 72) {
          float previous[2] = {};
          for (unsigned i = 0; i < count; ++i) {
            const auto* source = original.data() + size_t(i) * size;
            for (unsigned j = 0; j < 2; ++j) {
              volatile float speed = read_float(source + (j ? 56 : (source[46] ? 52 : 68))) +
                                     read_float(source + 60 + 4 * j);
              volatile float next = previous[j] + speed;
              float prediction = std::isfinite(next) ? next : 0.f;
              uint32_t bits;
              memcpy(&bits, &prediction, 4);
              uint32_t actual = be32(source + 9 + 4 * j), delta = actual - bits;
              check(be32(records.data() + size_t(i) * size + 9 + 4 * j) ==
                        ((delta << 1) ^ (0u - (delta >> 31))),
                    "velocity scalar oracle");
              memcpy(&previous[j], &actual, 4);
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
  g_v15_format = false;
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
