#include "archive.h"
#include "record_codec.h"
#include "slp.h"
#include "parallel.h"
#include "platform.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <variant>

namespace slippack {

std::string archive_name(std::string_view name) {
  require(!name.empty() && name.front() != '/', "unsafe archive path");
  require(name.find_first_of("\\:\r\n") == std::string_view::npos &&
              name.find('\0') == std::string_view::npos,
          "unsafe archive path");
  std::string result;
  size_t at = 0;
  while (at < name.size()) {
    auto slash = name.find('/', at);
    auto part = name.substr(at, slash == name.npos ? name.size() - at : slash - at);
    require(part != "..", "unsafe archive path (parent traversal)");
    if (!part.empty() && part != ".") {
      if (!result.empty()) {
        result += '/';
      }
      result += part;
    }
    if (slash == name.npos) {
      break;
    }
    at = slash + 1;
  }
  require(!result.empty() && name.back() != '/', "unsafe archive path");
  return result;
}
static void validate_names(const std::vector<std::string> &names) {
  std::set<std::string> files(names.begin(), names.end());
  require(files.size() == names.size(), "duplicate archive path");
  for (const auto &name : names) {
    for (size_t slash = name.find('/'); slash != name.npos; slash = name.find('/', slash + 1)) {
      require(!files.contains(name.substr(0, slash)), "archive file/directory conflict");
    }
  }
}
class PhaseTimer {
  const PhaseObserver &observe_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

public:
  explicit PhaseTimer(const PhaseObserver &observe) : observe_(observe) {}
  void operator()(const char *phase) {
    auto now = std::chrono::steady_clock::now();
    if (observe_) {
      observe_(phase, std::chrono::duration<double>(now - start_).count());
    }
    start_ = now;
  }
};

// ── per-file compress / decompress ──────────────────────────────────────────
// Compare flat custom coding with a reversible PPC branch transform for
// opaque attachments such as appended Gecko codesets.
static void pack_blob(std::span<const uint8_t> data, int level, Buf &blob,
                      std::atomic<uint64_t> *stat) {
  size_t before = blob.size();
  Buf flat, ppc;
  custom_frame(data, flat, slippack_custom::Generic, level);
  if (data.size() >= 65536) {
    std::vector<uint8_t> xf(data.begin(), data.end());
    ppc_branch_xform(xf.data(), xf.size() / 4, false);
    custom_frame(xf, ppc, slippack_custom::Generic, level);
  }
  bool usePpc = !ppc.empty() && ppc.size() < flat.size();
  blob.put_u8(usePpc ? 4 : 2);
  blob.put_u8(kCustom);
  const auto &winner = usePpc ? ppc : flat;
  blob.put_bytes(winner.data(), winner.size());
  if (stat) {
    *stat += blob.size() - before;
  }
}

// Modes 0, 2 and 4 are single custom frames; mode 3 is chunked below.
static std::vector<uint8_t> unpack_blob(Reader &r) {
  uint8_t mode = r.u8();
  if (mode == 0) {
    return custom_unframe(r);
  }
  if (mode != 2 && mode != 4) {
    fail("corrupt archive (blob mode)");
  }
  auto out = small_decompress(r);
  if (mode == 4) {
    ppc_branch_xform(out.data(), out.size() / 4, true);
  }
  return out;
}

// Phase 1 (parallel per file): parse, fold, group, write all structural bytes
// into an ordered piece list, deferring the entropy-heavy work as tasks.
// Phase 2 (parallel per TASK across files, largest first): pack groups/blobs.
// Phase 3 (serial): concatenate pieces per file, write the container.
// This removes the wall-clock pole of the largest file: work items are
// group-sized (~a few MB), and huge blobs are chunked (mode 3, 1 MB chunks).
struct FilePlan;

// Task payloads borrow a plan's buffers. Plans never move; buffers are retired
// only after the last task for that plan completes (pending's release/acquire).
struct RecordWork {
  uint32_t stream;
};
struct GroupWork {
  uint32_t stream, group;
};
struct BlobFrameWork {
  View data;
  bool ppc;
};
struct BlobTrialWork {
  View data;
};
struct RawWork {
  View data;
};
struct PackTask {
  FilePlan *fp;
  uint32_t piece;
  std::variant<RecordWork, GroupWork, BlobFrameWork, BlobTrialWork, RawWork> work;
  std::atomic<uint64_t> *stat;
  size_t est;
};

struct PooledStream {
  uint8_t cmd;
  uint32_t recSize;
  uint32_t count;
  Buf records;
};

struct FilePlan {
  const ArchiveInput *input = nullptr;
  Stats *stats = nullptr;
  std::string storedName;
  uint64_t origSize = 0;
  std::vector<uint8_t> data;
  ParsedSlp slp;
  std::vector<Buf> pieces;
  int curPiece = -1;
  std::vector<Buf> selectors;                 // per stream (grouped only)
  std::vector<std::vector<uint32_t>> gCounts; // per stream
  std::vector<PackTask> tasks;
  std::vector<PooledStream> pooled; // records moved out for pooling
  std::atomic<uint32_t> pending{0}; // unfinished tasks

  Buf &live() {
    if (curPiece < 0) {
      pieces.emplace_back();
      curPiece = (int)pieces.size() - 1;
    }
    return pieces[(size_t)curPiece];
  }
  uint32_t defer() {
    pieces.emplace_back();
    curPiece = -1;
    return (uint32_t)pieces.size() - 1;
  }
};

static void plan_blob(FilePlan &fp, std::span<const uint8_t> data, int level,
                      std::atomic<uint64_t> *stat) {
  // Small chunks keep large opaque attachments from becoming a
  // long final task after the replay streams have finished.
  constexpr size_t kChunk = 1u << 20;
  if (data.size() < 65536) {
    Buf &b = fp.live();
    size_t at = b.size();
    b.put_u8(0);
    custom_frame(data, b, slippack_custom::Generic, level);
    if (stat) {
      *stat += b.size() - at;
    }
  } else if (data.size() < kChunk) {
    fp.tasks.push_back({&fp, fp.defer(), BlobTrialWork{data}, stat, data.size()});
  } else {
    // decide the PPC transform once on a 256KB sample
    uint8_t xform = 0;
    {
      auto sample = data.first(std::min<size_t>(data.size(), 256 << 10));
      Buf a, bx;
      custom_frame(sample, a);
      std::vector<uint8_t> xf(sample.begin(), sample.end());
      ppc_branch_xform(xf.data(), xf.size() / 4, false);
      custom_frame(xf, bx);
      xform = bx.size() < a.size() ? 1 : 0;
    }
    uint32_t n = (uint32_t)((data.size() + kChunk - 1) / kChunk);
    Buf &b = fp.live();
    b.put_u8(3);
    b.put_u8(kCustom);
    b.put_u8(xform);
    b.put_u32(n);
    if (stat) {
      *stat += 7;
    }
    for (uint32_t i = 0; i < n; i++) {
      size_t off = (size_t)i * kChunk;
      auto chunk = data.subspan(off, std::min(kChunk, data.size() - off));
      fp.tasks.push_back({&fp, fp.defer(), BlobFrameWork{chunk, xform != 0}, stat, chunk.size()});
    }
  }
}

static void plan_compress(FilePlan &fp, int level) {
  auto *stats = fp.stats;
  fp.data = fp.input->load();
  fp.origSize = fp.data.size();
  if (!parse_slp(fp.data, fp.slp)) {
    fp.live().put_u8(kModeRaw);
    fp.tasks.push_back(
        {&fp, fp.defer(), RawWork{fp.data}, stats ? &stats->misc : nullptr, fp.data.size()});
    return;
  }
  {
    Buf &b = fp.live();
    b.put_u8(kModeSplit);
    b.put_bytes(fp.slp.prefix.data(), kSlpHeaderLen);
    b.put_u8((uint8_t)fp.slp.payloadsEvent.size());
    b.put_bytes(fp.slp.payloadsEvent.data(), fp.slp.payloadsEvent.size());
  }
  plan_blob(fp, fp.slp.suffix, level, stats ? &stats->suffix : nullptr);
  plan_blob(fp, fp.slp.tail, level, stats ? &stats->tail : nullptr);
  {
    Buf &b = fp.live();
    size_t before = b.size();
    small_compress(fp.slp.order, level, b, slippack_custom::Discrete);
    if (stats) {
      stats->order += b.size() - before;
      stats->misc += kSlpHeaderLen + 1 + fp.slp.payloadsEvent.size();
    }
  }
  fp.live().put_u8((uint8_t)fp.slp.streams.size());
  fp.selectors.resize(fp.slp.streams.size());
  fp.gCounts.resize(fp.slp.streams.size());
  EventStream *pre = nullptr, *post = nullptr;
  for (auto &st : fp.slp.streams) {
    if (st.cmd == 0x37) {
      pre = &st;
    }
    if (st.cmd == 0x38) {
      post = &st;
    }
  }
  bool xpred = false;
  if (pre && post && cross_pred_eligible(pre->recSize, pre->count, post->recSize, post->count)) {
    uint32_t pairs = 0;
    uint32_t hits =
        cross_pred_xor(pre->records.data(), pre->count, pre->recSize, post->records.data(),
                       post->count, post->recSize, /*probe=*/true, 128, &pairs);
    if (pairs >= 8 && hits * 2 >= pairs) {
      cross_pred_xor(pre->records.data(), pre->count, pre->recSize, post->records.data(),
                     post->count, post->recSize,
                     /*probe=*/false, 0, nullptr);
      xpred = true;
    }
  }
  static thread_local std::vector<int16_t> groupOf;
  for (uint32_t s = 0; s < fp.slp.streams.size(); s++) {
    auto &st = fp.slp.streams[s];
    auto *stat = stats ? &stats->byCmd[st.cmd] : nullptr;
    uint8_t flags = 0;
    if (xpred && st.cmd == 0x37) {
      flags |= 4;
    }
    Buf seedStream;
    uint32_t seedOff = seed_offset_for(st.cmd, st.recSize);
    // small side-event streams (game start, gecko lists, message splits...)
    // are pooled across all files: identical/near-identical content between
    // replays of the same setup becomes LZ matches in one shared buffer
    if (!splittable(st.cmd, st.recSize) && !seedOff && st.count > 0 &&
        st.records.size() < (128u << 10)) {
      Buf &b = fp.live();
      b.put_u8(st.cmd);
      b.put_u32(st.recSize);
      b.put_u32(st.count);
      b.put_u8(8); // flags: pooled
      fp.pooled.push_back({st.cmd, st.recSize, st.count, std::move(st.records)});
      st.count = 0;
      continue;
    }
    // group records by the 2-byte entity key at payload offsets 4-5
    // (before seed folding: the seed stream is emitted group-ordered)
    std::vector<uint16_t> keys;
    auto &counts = fp.gCounts[s];
    bool grouped = false;
    if (splittable(st.cmd, st.recSize) && st.count > 0) {
      groupOf.assign(65536, -1);
      grouped = true;
      for (uint32_t i = 0; i < st.count; i++) {
        const uint8_t *rec = st.records.data() + (size_t)i * st.recSize;
        uint16_t key = (uint16_t)(rec[4] << 8 | rec[5]);
        if (groupOf[key] < 0) {
          if (keys.size() >= 255) {
            grouped = false;
            counts.clear();
            break;
          }
          groupOf[key] = (int16_t)keys.size();
          keys.push_back(key);
          counts.push_back(0);
        }
        counts[(size_t)groupOf[key]]++;
      }
    }
    auto &selector = fp.selectors[s];
    if (grouped) {
      flags |= 1;
      selector.reserve(st.count);
      for (uint32_t i = 0; i < st.count; i++) {
        const uint8_t *rec = st.records.data() + (size_t)i * st.recSize;
        selector.put_u8((uint8_t)groupOf[(uint16_t)(rec[4] << 8 | rec[5])]);
      }
    }
    if (grouped) {
      if (st.cmd == 0x37 && st.recSize >= 59) {
        flags |= 32;
      }
      if (st.cmd == 0x38 && st.recSize >= 29u) {
        flags |= 16;
      }
    }
    if (seedOff &&
        fold_seeds(st, seedOff, seedStream, grouped ? selector.data() : nullptr, stats)) {
      flags |= 2;
    }
    {
      Buf &b = fp.live();
      b.put_u8(st.cmd);
      b.put_u32(st.recSize);
      b.put_u32(st.count);
      b.put_u8(flags);
      if (flags & 2) {
        size_t before = b.size();
        small_compress(seedStream, level, b, slippack_custom::Rng);
        if (stats) {
          stats->seeds += b.size() - before;
        }
      }
    }
    if (!grouped) {
      fp.tasks.push_back({&fp, fp.defer(), RecordWork{s}, stat, st.records.size()});
      continue;
    }
    {
      Buf &b = fp.live();
      size_t before = b.size();
      small_compress(selector, level, b, slippack_custom::Discrete);
      if (stats) {
        stats->selector += b.size() - before;
      }
      b.put_u8((uint8_t)keys.size());
    }
    for (uint32_t g = 0; g < keys.size(); g++) {
      fp.tasks.push_back({&fp, fp.defer(), GroupWork{s, g}, stat, (size_t)counts[g] * st.recSize});
    }
  }
}
static void run_pack_task(PackTask &t, int level) {
  Buf &out = t.fp->pieces[t.piece];
  std::visit(
      [&](const auto &work) {
        using Work = std::decay_t<decltype(work)>;
        if constexpr (std::is_same_v<Work, RawWork>) {
          auto n = custom_frame(work.data, out, slippack_custom::Generic, level);
          if (t.stat) {
            *t.stat += n;
          }
        } else if constexpr (std::is_same_v<Work, BlobTrialWork>) {
          pack_blob(work.data, level, out, t.stat);
        } else if constexpr (std::is_same_v<Work, BlobFrameWork>) {
          size_t at = out.size();
          if (work.ppc) {
            Bytes transformed(work.data.begin(), work.data.end());
            ppc_branch_xform(transformed.data(), transformed.size() / 4, false);
            custom_frame(transformed, out, slippack_custom::Generic, level);
          } else {
            custom_frame(work.data, out, slippack_custom::Generic, level);
          }
          if (t.stat) {
            *t.stat += out.size() - at;
          }
        } else {
          auto &st = t.fp->slp.streams[work.stream];
          if constexpr (std::is_same_v<Work, RecordWork>) {
            pack_records(st.records, st.count, st.recSize, level, out, t.stat, st.cmd);
          } else {
            auto &sel = t.fp->selectors[work.stream];
            uint32_t count = t.fp->gCounts[work.stream][work.group];
            Buf group;
            group.reserve(size_t(count) * st.recSize);
            for (uint32_t i = 0; i < st.count; ++i) {
              if (sel[i] == work.group) {
                group.put_bytes(st.records.data() + size_t(i) * st.recSize, st.recSize);
              }
            }
            out.put_u32(count);
            if (st.cmd == 0x37) {
              input_predict(group.data(), count, st.recSize, false);
            }
            if (st.cmd == 0x38) {
              motion_predict(group.data(), count, st.recSize, false);
            }
            pack_records(group, count, st.recSize, level, out, t.stat, st.cmd);
          }
        }
      },
      t.work);
}

EncodeResult encode_archive(std::span<const ArchiveInput> inputs, const EncodeOptions &options) {
  validate_threads(options.threads);
  require(options.admission_files > 0, "admission window must be positive");
  require(options.level >= 1 && options.level <= 12, "level must be between 1 and 12");
  narrow(inputs.size());
  RoundingGuard rounding;
  const auto [level, threads] = std::pair{options.level, options.threads};
  Stats counters;
  Stats *stats = options.collect_stats ? &counters : nullptr;
  std::vector<FilePlan> plans(inputs.size());
  std::vector<std::string> names;
  for (size_t i = 0; i < inputs.size(); ++i) {
    require(bool(inputs[i].load), "missing input loader");
    plans[i].input = &inputs[i];
    plans[i].stats = stats;
    plans[i].storedName = inputs[i].name;
    narrow(plans[i].storedName.size());
    names.push_back(archive_name(plans[i].storedName));
  }
  validate_names(names);
  PhaseTimer lap(options.on_phase);
  // pipelined: workers drain the pack heap (largest first) and plan more
  // files whenever no pack work is ready — no barrier between the phases.
  // A sliding admission window bounds memory: a file's parse/group buffers
  // are freed as soon as its last task retires.
  const size_t kWindow = options.admission_files;
  {
    std::mutex m;
    std::vector<PackTask *> ready;
    auto heapCmp = [](PackTask *a, PackTask *b) { return a->est < b->est; };
    size_t nextFile = 0;
    std::atomic<size_t> planned{0}, retired{0};
    auto retire = [&](FilePlan &p) {
      p.data = {};
      p.data.shrink_to_fit();
      p.slp = ParsedSlp();
      p.selectors.clear();
      p.gCounts.clear();
      retired.fetch_add(1);
    };
    auto worker = [&](FailureState &failure) {
      while (!failure.stopped()) {
        PackTask *t = nullptr;
        size_t fi = SIZE_MAX;
        {
          std::lock_guard<std::mutex> g(m);
          if (!ready.empty()) {
            std::pop_heap(ready.begin(), ready.end(), heapCmp);
            t = ready.back();
            ready.pop_back();
          } else if (nextFile < plans.size() && nextFile - retired.load() < kWindow) {
            fi = nextFile++;
          }
        }
        if (t) {
          run_pack_task(*t, level);
          if (t->fp->pending.fetch_sub(1) == 1) {
            retire(*t->fp);
          }
        } else if (fi != SIZE_MAX) {
          plan_compress(plans[fi], level);
          plans[fi].pending.store((uint32_t)plans[fi].tasks.size());
          if (plans[fi].tasks.empty()) {
            retire(plans[fi]);
          } else {
            std::lock_guard<std::mutex> g(m);
            for (auto &tk : plans[fi].tasks) {
              ready.push_back(&tk);
              std::push_heap(ready.begin(), ready.end(), heapCmp);
            }
          }
          planned.fetch_add(1);
        } else if (planned.load() < plans.size()) {
          std::this_thread::yield(); // planners/window in flight
        } else {
          return;
        }
      }
    };
    run_workers(threads, worker);
  }
  lap("plan+pack");

  // cross-replay pools: concatenate same-(cmd, recSize) small streams from
  // every file, in file order, and pack each pool once
  struct Pool {
    uint8_t cmd;
    uint32_t recSize;
    uint32_t total = 0;
    Buf records;
    Buf packed;
  };
  std::vector<Pool> pools;
  for (auto &p : plans) {
    for (auto &ps : p.pooled) {
      Pool *pl = nullptr;
      for (auto &q : pools) {
        if (q.cmd == ps.cmd && q.recSize == ps.recSize) {
          pl = &q;
          break;
        }
      }
      if (!pl) {
        pools.push_back({ps.cmd, ps.recSize, 0, {}, {}});
        pl = &pools.back();
      }
      require(ps.count <= UINT32_MAX - pl->total, "too many pooled records");
      pl->total += ps.count;
      pl->records.put_bytes(ps.records.data(), ps.records.size());
      ps.records = Buf();
    }
  }
  parallel_for(pools.size(), threads, [&](size_t i) {
    auto &pl = pools[i];
    pack_records(pl.records, pl.total, pl.recSize, level, pl.packed,
                 stats ? &stats->byCmd[pl.cmd] : nullptr, pl.cmd);
    pl.records = Buf();
  });
  lap("pools");

  Buf out;
  out.put_u32(kMagic);
  out.put_u8(kVersion);
  out.put_u8(1); // custom payload backend
  out.put_u32((uint32_t)pools.size());
  for (auto &pl : pools) {
    out.put_u8(pl.cmd);
    out.put_u32(pl.recSize);
    out.put_u32(pl.total);
    out.put_bytes(pl.packed.data(), pl.packed.size());
  }
  out.put_u32((uint32_t)plans.size());
  uint64_t totalIn = 0;
  for (auto &p : plans) {
    uint64_t blobLen = 0;
    for (auto &piece : p.pieces) {
      blobLen += piece.size();
    }
    out.put_u32((uint32_t)p.storedName.size());
    out.put_bytes(p.storedName.data(), p.storedName.size());
    out.put_u64(p.origSize);
    out.put_u64(blobLen);
    for (auto &piece : p.pieces) {
      out.put_bytes(piece.data(), piece.size());
    }
    totalIn += p.origSize;
  }
  lap("assemble");
  Statistics snapshot;
  if (stats) {
    for (unsigned c = 0; c < 256; ++c) {
      snapshot.by_command[c] = stats->byCmd[c].load();
    }
    snapshot.order = stats->order.load();
    snapshot.suffix = stats->suffix.load();
    snapshot.tail = stats->tail.load();
    snapshot.selector = stats->selector.load();
    snapshot.misc = stats->misc.load();
    snapshot.seeds = stats->seeds.load();
    snapshot.seed_hits = stats->seedHit.load();
    snapshot.seed_misses = stats->seedMiss.load();
  }
  return {std::move(out), totalIn, snapshot};
}

// ── decompress: 3 phases ────────────────────────────────────────────────────
// Phase 1 (parallel per file): walk the blob structure, decode the small side
// streams inline, and record byte spans for every packed section / blob frame.
// Phase 2 (parallel per task, largest first): decode spans into records.
// Phase 3 (parallel per file): gather source/prediction pointers, restore
// seeds and cross-frame residuals during final reconstruction, verify, write.
struct BlobPlan {
  uint8_t mode = 0, xform = 0;
  std::span<const uint8_t> span;                // modes 0/2/4, including mode byte
  std::vector<std::span<const uint8_t>> chunks; // mode 3 frames
  std::vector<std::vector<uint8_t>> chunkOut;
  std::vector<uint8_t> out;
};

struct DecStream {
  uint8_t cmd = 0;
  uint32_t recSize = 0, count = 0;
  uint8_t flags = 0;
  int poolIdx = -1;
  uint64_t poolOff = 0; // record offset of this file's slice in the pool
  View seedFrame, selectorFrame;
  std::vector<uint8_t> seedStream, selector;
  std::vector<uint32_t> gCounts;
  std::vector<std::span<const uint8_t>> spans; // 1 if ungrouped, else per group
  std::vector<OutputBytes> recs;               // task outputs
};

struct DecPlan {
  std::string storedName;
  uint64_t origSize = 0, sideBytes = 0;
  std::span<const uint8_t> blob;
  bool raw = false;
  std::span<const uint8_t> rawFrame;
  std::span<const uint8_t> prefix, ep;
  BlobPlan suffix, tail;
  View orderFrame;
  std::vector<uint8_t> order;
  std::vector<DecStream> streams;
  OutputBytes outData;
  std::atomic<uint32_t> pending{0};
  void release() {
    // note: `vec = {}` binds the initializer_list overload — it empties the
    // vector but KEEPS capacity; swap with a temporary actually deallocates
    OutputBytes().swap(outData);
    streams.clear();
    streams.shrink_to_fit();
    std::vector<uint8_t>().swap(order);
    suffix = BlobPlan();
    tail = BlobPlan();
  }
};

struct DecPool {
  uint8_t cmd = 0;
  uint32_t recSize = 0, total = 0;
  std::span<const uint8_t> span;
  OutputBytes records;
  uint64_t cursor = 0;
};

class MetadataBudget {
  std::atomic<uint64_t> used_{0};
  uint64_t limit_;

public:
  explicit MetadataBudget(uint64_t limit) : limit_(limit) {}
  void release(uint64_t bytes) { used_.fetch_sub(bytes); }
  void claim(uint64_t bytes) {
    auto used = used_.load();
    do {
      require(used <= limit_ && bytes <= limit_ - used, "archive exceeds metadata limit");
    } while (!used_.compare_exchange_weak(used, used + bytes));
  }
};
static uint64_t scan_blob(Reader &r, BlobPlan &bp, MetadataBudget &metadata) {
  const auto *start = r.p;
  bp.mode = r.u8();
  uint64_t output = 0;
  if (bp.mode == 0 || bp.mode == 2 || bp.mode == 4) {
    if (bp.mode) {
      require(r.u8() == kCustom, "corrupt blob codec");
    }
    output = frame_size(skip_frame(r));
    bp.span = {start, size_t(r.p - start)};
  } else if (bp.mode == 3) {
    require(r.u8() == kCustom, "corrupt blob codec");
    bp.xform = r.u8();
    require(bp.xform <= 1, "corrupt blob transform");
    uint32_t chunks = r.u32();
    require(chunks <= r.remaining() / 8, "corrupt blob chunk count");
    // Includes chunk descriptors, outputs, and deferred worker closures.
    metadata.claim(uint64_t(chunks) * (sizeof(View) + sizeof(Bytes) + 80));
    bp.chunks.reserve(chunks);
    for (uint32_t i = 0; i < chunks; ++i) {
      auto frame = skip_frame(r);
      output += frame_size(frame);
      bp.chunks.push_back(frame);
    }
    bp.chunkOut.resize(bp.chunks.size());
  } else {
    fail("corrupt blob mode");
  }
  return output;
}

// Only descriptors are scanned up front. Side streams are decoded on admission
// to a wave, so metadata expansion does not scale with the whole archive.
static void scan_decompress(DecPlan &dp, const DecodeLimits &limits, MetadataBudget &metadata) {
  Reader r{dp.blob.data(), dp.blob.data() + dp.blob.size(),
           std::min(dp.origSize, limits.frame_bytes)};
  auto side_frame = [&]() {
    require(r.u8() == kCustom, "corrupt side-stream codec");
    auto frame = skip_frame(r);
    uint64_t bytes = frame_size(frame);
    dp.sideBytes += bytes;
    require(dp.sideBytes <= limits.metadata_bytes, "file exceeds metadata limit");
    return frame;
  };
  uint8_t mode = r.u8();
  if (mode == kModeRaw) {
    dp.raw = true;
    dp.rawFrame = skip_frame(r);
    require(frame_size(dp.rawFrame) == dp.origSize, "corrupt raw file size");
    r.done();
    return;
  }
  require(mode == kModeSplit, "corrupt archive mode");
  dp.prefix = r.bytes(kSlpHeaderLen);
  dp.ep = r.bytes(r.u8());
  uint64_t output = kSlpHeaderLen + 1 + dp.ep.size();
  output += scan_blob(r, dp.suffix, metadata);
  output += scan_blob(r, dp.tail, metadata);
  dp.orderFrame = side_frame();
  uint64_t events = 0;
  std::array<bool, 256> seen{};
  auto streamCount = r.u8();
  metadata.claim(uint64_t(streamCount) * (sizeof(DecStream) + 160));
  dp.streams.resize(streamCount);
  for (auto &st : dp.streams) {
    st.cmd = r.u8();
    st.recSize = r.u32();
    st.count = r.u32();
    st.flags = r.u8();
    require(!seen[st.cmd], "corrupt duplicate stream");
    seen[st.cmd] = true;
    require(st.recSize <= UINT16_MAX && st.count <= limits.records, "record shape exceeds limit");
    require((st.flags & ~63u) == 0 && (!(st.flags & 8) || st.flags == 8), "corrupt stream flags");
    require(!(st.flags & 2) || seed_offset_for(st.cmd, st.recSize), "corrupt seed flag");
    require(!(st.flags & 16) || (st.cmd == 0x38 && (st.flags & 1) && st.recSize >= 29),
            "corrupt motion predictor flag");
    require(!(st.flags & 32) || (st.cmd == 0x37 && (st.flags & 1) && st.recSize >= 59),
            "corrupt input predictor flag");
    events += st.count;
    output += uint64_t(st.recSize + 1) * st.count;
    require(output <= dp.origSize && events <= limits.records, "corrupt record output size");
    if (st.flags & 8) {
      continue;
    }
    if (st.flags & 2) {
      st.seedFrame = side_frame();
      require(frame_size(st.seedFrame) <= uint64_t(st.count) * 6, "corrupt seed stream size");
    }
    if (!(st.flags & 1)) {
      st.spans.push_back(skip_packed(r, st.recSize, st.count));
      st.gCounts.push_back(st.count);
    } else {
      st.selectorFrame = side_frame();
      require(frame_size(st.selectorFrame) == st.count, "corrupt selector size");
      uint8_t groups = r.u8();
      metadata.claim(uint64_t(groups) * (sizeof(View) + sizeof(OutputBytes) + 96));
      uint64_t count = 0;
      for (unsigned g = 0; g < groups; ++g) {
        auto n = r.u32();
        count += n;
        require(count <= st.count, "corrupt group count");
        st.gCounts.push_back(n);
        st.spans.push_back(skip_packed(r, st.recSize, n));
      }
      require(count == st.count, "corrupt group count");
    }
    st.recs.resize(st.spans.size());
  }
  require(output == dp.origSize && frame_size(dp.orderFrame) == events, "corrupt file output size");
  r.done();
}
static void prepare_file(DecPlan &dp, MetadataBudget &metadata) {
  if (dp.raw) {
    return;
  }
  metadata.claim(dp.sideBytes);
  Reader order(dp.orderFrame);
  dp.order = custom_unframe(order);
  for (auto &stream : dp.streams) {
    if (!stream.seedFrame.empty()) {
      Reader seeds(stream.seedFrame);
      stream.seedStream = custom_unframe(seeds);
    }
    if (!stream.selectorFrame.empty()) {
      Reader selector(stream.selectorFrame);
      stream.selector = custom_unframe(selector);
    }
  }
}

// Factories bind the exact work and owner together, without nullable payload
// unions. Each output buffer has one writer until the owner's last task retires.
struct DecTask {
  DecPlan *owner;
  std::function<void()> run;
  size_t est;
  DecTask(DecPlan &plan, std::function<void()> work, size_t estimate)
      : owner(&plan), run(std::move(work)), est(estimate) {}
  static DecTask raw(DecPlan &plan) {
    return {plan,
            [&plan] {
              Reader r(plan.rawFrame);
              auto bytes = custom_unframe(r);
              plan.outData.assign(bytes.begin(), bytes.end());
            },
            plan.rawFrame.size()};
  }
  static DecTask blob(DecPlan &plan, BlobPlan &blob) {
    return {plan,
            [&blob] {
              Reader r(blob.span);
              blob.out = unpack_blob(r);
            },
            blob.span.size()};
  }
  static DecTask chunk(DecPlan &plan, BlobPlan &blob, uint32_t index) {
    return {plan,
            [&blob, index] {
              Reader r(blob.chunks[index]);
              auto &out = blob.chunkOut[index];
              out = custom_unframe(r);
              if (blob.xform) {
                ppc_branch_xform(out.data(), out.size() / 4, true);
              }
            },
            blob.chunks[index].size()};
  }
  static DecTask records(DecPlan &plan, DecStream &stream, uint32_t group) {
    return {plan,
            [&stream, group] {
              Reader r(stream.spans[group]);
              auto count = stream.gCounts[group];
              auto &out = stream.recs[group];
              unpack_records(r, count, stream.recSize, out);
              if (stream.flags & 16) {
                motion_predict(out.data(), count, stream.recSize, true);
              }
              if (stream.flags & 32) {
                input_predict(out.data(), count, stream.recSize, true);
              }
            },
            size_t(stream.gCounts[group]) * stream.recSize};
  }
};

static void finish_blob(BlobPlan &bp) {
  if (bp.mode != 3) {
    return;
  }
  size_t total = 0;
  for (auto &c : bp.chunkOut) {
    total += c.size();
  }
  bp.out.reserve(total);
  for (auto &c : bp.chunkOut) {
    bp.out.insert(bp.out.end(), c.begin(), c.end());
  }
}

// Copy grouped records straight into their final event positions. Small
// source/prediction pointers let RNG and cross-frame restoration happen while
// the destination record is still hot in cache.
static void assemble_decompress(DecPlan &dp, std::vector<DecPool> &pools) {
  if (dp.raw) {
    if (dp.outData.size() != dp.origSize) {
      fail("size mismatch for " + dp.storedName);
    }
    return;
  }
  finish_blob(dp.suffix);
  finish_blob(dp.tail);
  int16_t idxOf[256];
  for (auto &i : idxOf) {
    i = -1;
  }
  struct Source {
    std::vector<const uint8_t *> records, predictions;
    std::vector<size_t> seedCursors;
    uint32_t at = 0, seed = 0, seedOffset = 0;
  };
  std::vector<Source> sources(dp.streams.size());
  size_t rawSize = 1 + dp.ep.size() + dp.tail.out.size();
  for (size_t j = 0; j < dp.streams.size(); ++j) {
    auto &st = dp.streams[j];
    auto &src = sources[j];
    if (idxOf[st.cmd] >= 0) {
      fail("corrupt archive (duplicate stream)");
    }
    idxOf[st.cmd] = int16_t(j);
    rawSize += size_t(st.recSize) * st.count + st.count;
    src.records.resize(st.count);
    if (st.flags & 8) {
      if (st.poolIdx < 0) {
        fail("corrupt archive (pool ref)");
      }
      auto &pl = pools[size_t(st.poolIdx)];
      const uint8_t *base = pl.records.data();
      if (st.recSize) {
        base += st.poolOff * st.recSize;
      }
      for (unsigned i = 0; i < st.count; ++i) {
        src.records[i] = st.recSize ? base + size_t(i) * st.recSize : base;
      }
    } else {
      if (st.recs.size() != st.gCounts.size()) {
        fail("corrupt archive (groups)");
      }
      for (size_t g = 0; g < st.recs.size(); ++g) {
        if (st.recs[g].size() != size_t(st.gCounts[g]) * st.recSize) {
          fail("corrupt archive (group size)");
        }
      }
      std::vector<uint32_t> used(st.recs.size());
      for (unsigned i = 0; i < st.count; ++i) {
        unsigned g = st.flags & 1 ? st.selector[i] : 0;
        if (g >= used.size() || used[g] >= st.gCounts[g]) {
          fail("corrupt archive (selector count)");
        }
        const uint8_t *base = st.recs[g].data();
        src.records[i] = st.recSize ? base + size_t(used[g]) * st.recSize : base;
        ++used[g];
      }
    }
    if (st.flags & 2) {
      src.seedOffset = seed_offset_for(st.cmd, st.recSize);
      if (!src.seedOffset) {
        fail("corrupt archive (seed flag on seedless cmd)");
      }
      bool grouped = st.flags & 1;
      unsigned groups = grouped ? unsigned(st.gCounts.size()) : 1;
      src.seedCursors.resize(groups);
      size_t pos = 0;
      for (unsigned g = 0; g < groups; ++g) {
        src.seedCursors[g] = pos;
        unsigned n = grouped ? st.gCounts[g] : st.count;
        for (unsigned i = 0; i < n; ++i) {
          uint32_t k = get_varint(st.seedStream, pos);
          require(k <= kSeedException, "corrupt seed step count");
          if (k == kSeedException) {
            pos += 4;
          }
          if (pos > st.seedStream.size()) {
            fail("corrupt archive (seed stream)");
          }
        }
      }
      require(pos == st.seedStream.size(), "corrupt trailing seed bytes");
    }
  }
  for (size_t j = 0; j < dp.streams.size(); ++j) {
    auto &pre = dp.streams[j];
    if (!(pre.flags & 4)) {
      continue;
    }
    if (pre.cmd != 0x37 || idxOf[0x38] < 0) {
      fail("corrupt archive (xpred flag)");
    }
    unsigned postIdx = idxOf[0x38];
    auto &post = dp.streams[postIdx];
    if (!cross_pred_eligible(pre.recSize, pre.count, post.recSize, post.count)) {
      fail("corrupt archive (xpred shape)");
    }
    auto &predictions = sources[j].predictions;
    predictions.resize(pre.count, nullptr);
    static thread_local std::array<uint32_t, 65536> heads{};
    static thread_local std::array<bool, 65536> seen{};
    std::unique_ptr<uint32_t[]> next(new uint32_t[post.count]);
    std::vector<uint16_t> keys;
    keys.reserve(std::min<uint32_t>(65536, post.count));
    for (unsigned i = post.count; i-- > 0;) {
      const uint8_t *q = sources[postIdx].records[i];
      uint16_t k = uint16_t(q[4] << 8 | q[5]);
      if (!heads[k]) {
        keys.push_back(k);
        seen[k] = false;
      }
      next[i] = heads[k];
      heads[k] = i + 1;
    }
    for (unsigned i = 0; i < pre.count; ++i) {
      const uint8_t *p = sources[j].records[i];
      uint16_t k = uint16_t(p[4] << 8 | p[5]);
      if (!heads[k]) {
        continue;
      }
      if (!seen[k]) {
        seen[k] = true;
        continue;
      }
      unsigned at = heads[k] - 1;
      predictions[i] = sources[postIdx].records[at];
      heads[k] = next[at];
    }
    for (auto k : keys) {
      heads[k] = 0;
    }
  }
  auto &out = dp.outData;
  out.resize(dp.prefix.size() + rawSize + dp.suffix.out.size());
  uint8_t *dest = out.data();
  auto append = [&](std::span<const uint8_t> part) {
    if (!part.empty()) {
      memcpy(dest, part.data(), part.size());
    }
    dest += part.size();
  };
  append(dp.prefix);
  *dest++ = 0x35;
  append(dp.ep);
  for (uint8_t cmd : dp.order) {
    int16_t j = idxOf[cmd];
    if (j < 0) {
      fail("corrupt archive (order stream)");
    }
    auto &st = dp.streams[size_t(j)];
    auto &src = sources[size_t(j)];
    unsigned i = src.at++;
    if (i >= st.count) {
      fail("corrupt archive (order count)");
    }
    *dest++ = cmd;
    if (st.recSize) {
      memcpy(dest, src.records[i], st.recSize);
    }
    if (src.seedOffset) {
      unsigned g = st.flags & 1 ? st.selector[i] : 0;
      if (g >= src.seedCursors.size()) {
        fail("corrupt archive (seed group)");
      }
      auto &cursor = src.seedCursors[g];
      uint32_t k = get_varint(st.seedStream, cursor);
      if (k == kSeedException) {
        Reader seed(View(st.seedStream).subspan(cursor));
        src.seed = seed.u32();
        cursor += 4;
      } else {
        src.seed = lcg_advance(src.seed, k);
      }
      auto *p = dest + src.seedOffset;
      p[0] = src.seed >> 24;
      p[1] = src.seed >> 16;
      p[2] = src.seed >> 8;
      p[3] = src.seed;
    }
    if (!src.predictions.empty()) {
      if (const uint8_t *q = src.predictions[i]) {
        for (unsigned k = 0; k < kLenA; ++k) {
          dest[kPreA + k] ^= q[kPostA + k];
        }
        for (unsigned k = 0; k < kLenB; ++k) {
          dest[kPreB + k] ^= q[kPostB + k];
        }
      }
    }
    dest += st.recSize;
  }
  append(dp.tail.out);
  append(dp.suffix.out);
  if (dest != out.data() + out.size() || out.size() != dp.origSize) {
    fail("size mismatch for " + dp.storedName);
  }
  for (size_t j = 0; j < dp.streams.size(); ++j) {
    if (sources[j].at != dp.streams[j].count) {
      fail("corrupt archive (order count)");
    }
    dp.streams[j].recs.clear();
    dp.streams[j].recs.shrink_to_fit();
  }
}

DecodeResult decode_archive(View arc, const DecodeOptions &options, FileSink sink) {
  validate_threads(options.threads);
  require(options.wave_bytes > 0, "wave budget must be positive");
  require(arc.size() <= options.limits.archive_bytes, "archive exceeds input limit");
  RoundingGuard rounding;
  const int threads = options.threads;
  DecodeResult result;
  Reader r(arc);
  r.output_limit = options.limits.frame_bytes;
  if (r.u32() != kMagic) {
    fail("not a slippack archive");
  }
  uint8_t version = r.u8(), flags = r.u8();
  if (version != kVersion || flags != 1) {
    fail("unsupported slippack format (requires v1 with flags=1)");
  }
  MetadataBudget metadata(options.limits.metadata_bytes);
  uint32_t nPools = r.u32();
  require(nPools <= options.limits.pools && nPools <= r.remaining() / 19,
          "pool count exceeds limit or directory");
  metadata.claim(uint64_t(nPools) * (sizeof(DecPool) + 64));
  std::vector<DecPool> pools(nPools);
  uint64_t poolBytes = 0;
  std::set<std::pair<uint8_t, uint32_t>> poolKeys;
  for (auto &pl : pools) {
    pl.cmd = r.u8();
    pl.recSize = r.u32();
    pl.total = r.u32();
    require(pl.recSize <= UINT16_MAX, "pool record shape exceeds limit");
    require(poolKeys.emplace(pl.cmd, pl.recSize).second, "corrupt duplicate pool");
    auto bytes = uint64_t(pl.recSize) * pl.total;
    require(bytes <= options.limits.pool_bytes - poolBytes, "pools exceed output limit");
    poolBytes += bytes;
    pl.span = skip_packed(r, pl.recSize, pl.total);
  }
  uint32_t n = r.u32();
  require(n <= options.limits.files && n <= r.remaining() / 21,
          "file count exceeds limit or directory");
  metadata.claim(uint64_t(n) * (sizeof(DecPlan) + sizeof(EntryInfo) + sizeof(std::string) + 192));
  std::vector<DecPlan> plans(n);
  result.entries.reserve(n);
  std::vector<std::string> names;
  for (auto &dp : plans) {
    uint32_t nameLen = r.u32();
    require(nameLen <= options.limits.name_bytes, "archive name exceeds limit");
    metadata.claim(uint64_t(nameLen) * 4);
    auto name = r.bytes(nameLen);
    dp.origSize = r.u64();
    require(dp.origSize <= options.limits.file_bytes &&
                dp.origSize <= options.limits.total_bytes - result.original_bytes,
            "archive exceeds decoded size limit");
    uint64_t blobLen = r.u64();
    auto blob = r.bytes(blobLen);
    dp.storedName.assign(reinterpret_cast<const char *>(name.data()), name.size());
    names.push_back(archive_name(dp.storedName));
    result.entries.push_back({dp.storedName, dp.origSize, {}});
    result.original_bytes += dp.origSize;
    dp.blob = blob;
  }

  r.done();
  validate_names(names);
  PhaseTimer lap(options.on_phase);
  parallel_for(plans.size(), threads,
               [&](size_t i) { scan_decompress(plans[i], options.limits, metadata); });
  lap("scan");

  // resolve pool slices in file order; each pooled stream is a dependency
  for (auto &dp : plans) {
    if (dp.raw) {
      continue;
    }
    for (auto &st : dp.streams) {
      if (!(st.flags & 8)) {
        continue;
      }
      DecPool *pl = nullptr;
      for (size_t i = 0; i < pools.size(); i++) {
        if (pools[i].cmd == st.cmd && pools[i].recSize == st.recSize) {
          st.poolIdx = (int)i;
          pl = &pools[i];
          break;
        }
      }
      if (!pl || pl->cursor + st.count > pl->total) {
        fail("corrupt archive (pool slice)");
      }
      st.poolOff = pl->cursor;
      pl->cursor += st.count;
    }
  }

  // decode pools up front: they are small, live for the whole run, and every
  // wave below may reference them — no per-file dependency accounting needed
  for (const auto &pool : pools) {
    require(pool.cursor == pool.total, "corrupt unused pool records");
  }
  parallel_for(pools.size(), threads, [&](size_t i) {
    auto &pool = pools[i];
    Reader r(pool.span);
    unpack_records(r, pool.total, pool.recSize, pool.records);
  });
  lap("pools");

  auto finish = [&](DecPlan &dp) {
    assemble_decompress(dp, pools);
    auto &entry = result.entries[size_t(&dp - plans.data())];
    if (options.compute_hashes) {
      entry.sha1 = sha1(dp.outData);
    }
    if (sink) {
      sink({dp.storedName, std::move(dp.outData)});
    }
    dp.release();
    metadata.release(dp.sideBytes);
  };
  // admission waves: only ~kDecBudget of decoded output is in flight at once,
  // so peak memory stays bounded instead of scaling with archive raw size
  const uint64_t kDecBudget = options.wave_bytes;
  size_t next = 0;
  while (next < plans.size()) {
    size_t first = next;
    uint64_t bytes = 0;
    while (next < plans.size() && (next == first || bytes < kDecBudget)) {
      bytes += plans[next++].origSize;
    }
    parallel_for(next - first, threads,
                 [&](size_t i) { prepare_file(plans[first + i], metadata); });
    std::vector<DecTask> tasks;
    for (size_t index = first; index < next; ++index) {
      auto &dp = plans[index];
      size_t before = tasks.size();
      if (dp.raw) {
        tasks.push_back(DecTask::raw(dp));
      } else {
        for (auto *bp : {&dp.suffix, &dp.tail}) {
          if (bp->mode == 3) {
            for (uint32_t i = 0; i < bp->chunks.size(); ++i) {
              tasks.push_back(DecTask::chunk(dp, *bp, i));
            }
          } else if (!bp->span.empty()) {
            tasks.push_back(DecTask::blob(dp, *bp));
          }
        }
        for (auto &stream : dp.streams) {
          for (uint32_t group = 0; group < stream.spans.size(); ++group) {
            tasks.push_back(DecTask::records(dp, stream, group));
          }
        }
      }
      if (tasks.size() == before) {
        tasks.emplace_back(dp, [] {}, 0);
      }
      dp.pending.store(narrow(tasks.size() - before));
    }
    std::sort(tasks.begin(), tasks.end(),
              [](const DecTask &a, const DecTask &b) { return a.est > b.est; });
    parallel_for(tasks.size(), threads, [&](size_t i) {
      auto &t = tasks[i];
      t.run();
      if (t.owner->pending.fetch_sub(1) == 1) {
        finish(*t.owner);
      }
    });
    if (next < plans.size()) {
      release_unused_memory();
    }
  }
  lap("decode+assemble");
  return result;
}

} // namespace slippack
