#pragma once
#include "format.h"

namespace slpz {
constexpr uint32_t kSeedCap = 4096; // max steps to search
inline uint32_t lcg_next(uint32_t s) {
  return s * 214013u + 2531011u;
}

struct LcgTables {
  std::array<uint16_t, 8192> phase{};
  struct Jump {
    uint32_t multiply, add;
  };
  std::array<Jump, kSeedCap + 1> jumps{};
  LcgTables() {
    uint32_t seed = 0;
    for (unsigned i = 0; i < phase.size(); ++i) {
      phase[seed & 8191] = uint16_t(i);
      seed = lcg_next(seed);
    }
    jumps[0] = {1, 0};
    for (unsigned k = 1; k < jumps.size(); ++k)
      jumps[k] = {jumps[k - 1].multiply * 214013u, lcg_next(jumps[k - 1].add)};
  }
};
struct EventStream {
  uint8_t cmd = 0;
  uint32_t recSize = 0; // payload bytes per record (command byte excluded)
  Buf records;          // concatenated payloads
  uint32_t count = 0;
};

// Prefix/suffix/tail views borrow the input; stream records and order are owned.
struct ParsedSlp {
  std::span<const uint8_t> prefix, suffix, tail;
  std::span<const uint8_t> payloadsEvent; // 0x35 payload, verbatim
  Buf order;                              // command byte per event
  std::vector<EventStream> streams;
};

constexpr uint32_t kSeedException = kSeedCap + 1;

constexpr uint32_t kPreA = 10, kPostA = 7, kLenA = 14;
constexpr uint32_t kPreB = 59, kPostB = 21, kLenB = 4;

// Preferred complete 4-byte fields, in v1 encoder selection order.
std::span<const uint16_t> semantic_fields(uint8_t cmd);
uint8_t field_profile(uint8_t cmd, uint32_t offset, bool dictionary);
uint32_t seed_offset_for(uint8_t cmd, uint32_t recSize);
const LcgTables &lcg_tables();
uint32_t lcg_advance(uint32_t seed, uint32_t steps);
uint32_t lcg_distance(uint32_t a, uint32_t b);
// Resets out on entry. On false, discard partial state and encode the input raw.
bool parse_slp(std::span<const uint8_t> file, ParsedSlp &out);
bool splittable(uint8_t cmd, uint32_t recSize);
bool fold_seeds(EventStream &st, uint32_t seedOff, Buf &seedStream, const uint8_t *groupSel,
                Stats *stats = nullptr);
bool cross_pred_eligible(uint32_t preSize, uint32_t preCount, uint32_t postSize,
                         uint32_t postCount);
uint32_t cross_pred_xor(uint8_t *pre, uint32_t preCount, uint32_t preSize, const uint8_t *post,
                        uint32_t postCount, uint32_t postSize, bool probe, uint32_t probeMax,
                        uint32_t *pairsOut);
void ppc_branch_xform(uint8_t *p, size_t nWords, bool inverse);
float read_float(const uint8_t *p);
uint32_t integer_grid(uint32_t value);
void motion_predict(uint8_t *data, uint32_t count, uint32_t size, bool inverse);
uint32_t controller_grid(uint32_t value);
void input_predict(uint8_t *data, uint32_t count, uint32_t size, bool inverse);

} // namespace slpz
