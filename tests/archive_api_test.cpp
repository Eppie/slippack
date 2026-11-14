#include "../archive.h"
#include "../frames.h"
#include "../parallel.h"
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <set>

using namespace slippack;
static size_t checks = 0;
static void check(bool ok, const char *why) {
  if (!ok) {
    std::fprintf(stderr, "archive API test: %s\n", why);
    std::abort();
  }
  ++checks;
}
template <class Fn> static void rejects(Fn fn, std::string_view message = {}) {
  try {
    fn();
  } catch (const Error &error) {
    check(std::string_view(error.what()).find(message) != std::string_view::npos, "wrong error");
    return;
  }
  check(false, "expected an exception");
}
static Bytes raw_archive(std::string_view name, View payload, uint64_t size) {
  Buf out;
  out.put_u32(kMagic);
  out.put_u8(1);
  out.put_u8(1);
  out.put_u32(0);
  out.put_u32(1);
  out.put_u32(narrow(name.size()));
  out.put_bytes(name.data(), name.size());
  out.put_u64(size);
  Buf body;
  body.put_u8(kModeRaw);
  custom_frame(payload, body);
  out.put_u64(body.size());
  out.put_bytes(body.data(), body.size());
  return out;
}
int main() {
  Bytes payload{'a', 'b', 'c'};
  std::vector<ArchiveInput> inputs;
  std::atomic<unsigned> loads{0};
  for (unsigned i = 0; i < 80; ++i) {
    inputs.push_back({"folder/file-" + std::to_string(i), [&] {
                        ++loads;
                        return payload;
                      }});
  }
  EncodeOptions encode;
  encode.threads = 4;
  encode.collect_stats = true;
  encode.admission_files = 2;
  auto packed = encode_archive(inputs, encode);
  check(loads == inputs.size(), "load exactly once per input");
  check(packed.bytes[4] == 1, "format remains v1");
  DecodeOptions decode;
  decode.threads = 4;
  decode.compute_hashes = true;
  decode.wave_bytes = 10;
  std::mutex mutex;
  std::set<std::string> names;
  auto result = decode_archive(packed.bytes, decode, [&](DecodedFile file) {
    if (!std::equal(file.bytes.begin(), file.bytes.end(), payload.begin(), payload.end())) {
      std::abort();
    }
    std::lock_guard lock(mutex);
    if (!names.insert(file.name).second) {
      std::abort();
    }
  });
  check(names.size() == inputs.size() && result.original_bytes == 240, "owned sink output");
  check(result.entries[0].sha1 == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1 result");
  // Failures propagate from every kind of callback; workers must have joined.
  auto broken = inputs;
  broken[1].load = []() -> Bytes { throw Error("loader failure"); };
  rejects([&] { encode_archive(broken, encode); }, "loader failure");
  rejects(
      [&] {
        decode_archive(packed.bytes, decode, [](DecodedFile) { throw Error("sink failure"); });
      },
      "sink failure");
  auto observer = encode;
  observer.on_phase = [](std::string_view, double) { throw Error("observer failure"); };
  rejects([&] { encode_archive(inputs, observer); }, "observer failure");
  // Multiple callers and recursive callbacks share no mutable operation state.
  auto operation = [&] {
    auto run = encode_archive(inputs, encode);
    if (run.bytes != packed.bytes || run.statistics.misc != packed.statistics.misc) {
      std::abort();
    }
    auto restored = decode_archive(run.bytes, decode);
    if (restored.entries.size() != inputs.size()) {
      std::abort();
    }
  };
  auto a = std::async(std::launch::async, operation), b = std::async(std::launch::async, operation);
  operation();
  a.get();
  b.get();
  ++checks;
  std::vector<ArchiveInput> recursive{{"recursive", [&] {
                                         auto inner = decode_archive(packed.bytes);
                                         if (inner.entries.size() != inputs.size()) {
                                           std::abort();
                                         }
                                         return payload;
                                       }}};
  auto nested = encode_archive(recursive);
  check(decode_archive(nested.bytes).entries.size() == 1, "recursive caller");
  std::fesetround(FE_DOWNWARD);
  check(encode_archive(inputs, encode).bytes == packed.bytes, "rounding independence");
  check(std::fegetround() == FE_DOWNWARD, "caller rounding restored");
  std::fesetround(FE_TONEAREST);

  // One strict varint reader serves frames, payload coding and RNG streams.
  for (uint32_t value : {0u, 127u, 128u, 16384u, UINT32_MAX}) {
    Bytes bytes;
    varint(bytes, value);
    Reader reader(bytes);
    check(reader.var() == value, "valid varint");
    reader.done();
  }
  for (Bytes bytes : {Bytes{128}, Bytes{128, 128, 128, 128, 16}, Bytes{255, 255, 255, 255, 255}}) {
    rejects([&] {
      Reader reader(bytes);
      reader.var();
    });
  }
  Bytes little;
  u32(little, 0x12345678);
  check(little == Bytes({0x78, 0x56, 0x34, 0x12}), "explicit endian encoding");

  check(decode_archive(raw_archive("./one", payload, 3)).entries[0].name == "./one",
        "stored name preserved");
  auto tiny = raw_archive("one", payload, 3);
  for (size_t length = 0; length < tiny.size(); ++length) {
    rejects([&] { decode_archive(View(tiny).first(length)); });
  }
  auto trailing = tiny;
  trailing.push_back(0);
  rejects([&] { decode_archive(trailing); }, "trailing");
  for (auto member : {"../escape", "/absolute", "nested/../../escape", "C:drive", "folder\\file",
                      "line\nbreak"}) {
    rejects([&] { decode_archive(raw_archive(member, payload, 3)); }, "unsafe");
  }
  auto duplicate = inputs;
  duplicate[1].name = "folder/./file-0";
  rejects([&] { encode_archive(duplicate); }, "duplicate");
  duplicate[1].name = "folder";
  rejects([&] { encode_archive(duplicate); }, "conflict");

  // Check cheap limits before allocations or callbacks, including false sizes.
  auto limit = decode;
  limit.limits.archive_bytes = 1;
  rejects([&] { decode_archive(tiny, limit); }, "input limit");
  limit = decode;
  limit.limits.file_bytes = 2;
  rejects([&] { decode_archive(tiny, limit); }, "size limit");
  limit = decode;
  limit.limits.total_bytes = 2;
  rejects([&] { decode_archive(tiny, limit); }, "size limit");
  limit = decode;
  limit.limits.frame_bytes = 2;
  rejects([&] { decode_archive(tiny, limit); }, "frame");
  limit = decode;
  limit.limits.files = 0;
  rejects([&] { decode_archive(tiny, limit); }, "file count");
  rejects([&] { decode_archive(raw_archive("wrong", payload, 4)); }, "raw file size");
  auto impossible = tiny;
  for (unsigned i = 6; i < 10; ++i) {
    impossible[i] = 255;
  }
  rejects([&] { decode_archive(impossible); }, "pool count");
  impossible = tiny;
  for (unsigned i = 10; i < 14; ++i) {
    impossible[i] = 255;
  }
  rejects([&] { decode_archive(impossible); }, "file count");
  decode.threads = 0;
  rejects([&] { decode_archive(tiny, decode); }, "threads");
  encode.admission_files = 0;
  rejects([&] { encode_archive(inputs, encode); }, "admission");
  encode.admission_files = 64;
  encode.concurrent_loads = 0;
  rejects([&] { encode_archive(inputs, encode); }, "load limit");
  // Loaders never overlap beyond the limit, and the output does not depend on it.
  for (size_t limit : {size_t(1), size_t(2), size_t(64)}) {
    std::atomic<unsigned> active{0}, peak{0};
    std::vector<ArchiveInput> gated;
    for (unsigned i = 0; i < 80; ++i) {
      gated.push_back({"folder/file-" + std::to_string(i), [&] {
                         unsigned now = ++active, seen = peak.load();
                         while (now > seen && !peak.compare_exchange_weak(seen, now)) {
                         }
                         std::this_thread::sleep_for(std::chrono::microseconds(200));
                         --active;
                         return payload;
                       }});
    }
    encode.threads = 8;
    encode.concurrent_loads = limit;
    check(encode_archive(gated, encode).bytes == packed.bytes, "load limit changes output");
    check(peak >= 1 && peak <= std::min<size_t>(limit, 8), "concurrent loads exceed limit");
  }
  // Listing reads the directory alone and agrees with a complete decode.
  decode.threads = 4;
  auto listed = list_archive(packed.bytes);
  check(listed.entries.size() == inputs.size() && listed.original_bytes == 240, "listing totals");
  for (size_t i = 0; i < inputs.size(); ++i) {
    check(listed.entries[i].name == result.entries[i].name && listed.entries[i].size == 3 &&
              listed.entries[i].sha1.empty(),
          "listing entry");
  }
  check(list_archive(raw_archive("./one", payload, 3)).entries[0].name == "./one",
        "listing preserves stored name");
  for (size_t length = 0; length < tiny.size(); ++length) {
    rejects([&] { list_archive(View(tiny).first(length)); });
  }
  rejects([&] { list_archive(trailing); }, "trailing");
  rejects([&] { list_archive(raw_archive("../escape", payload, 3)); }, "unsafe");
  auto listLimits = decode.limits;
  listLimits.files = 0;
  rejects([&] { list_archive(tiny, listLimits); }, "file count");
  listLimits = decode.limits;
  listLimits.archive_bytes = 1;
  rejects([&] { list_archive(tiny, listLimits); }, "input limit");
  // A corrupt payload does not affect listing, which never reads it.
  auto damaged = tiny;
  damaged.back() ^= 255;
  check(list_archive(damaged).entries.size() == 1, "listing ignores payload bytes");

  // A selection decodes exactly the named files, in archive order, whatever
  // the order, spelling, or repetition of the request.
  auto select = decode;
  select.members = {"folder/file-7", "./folder/file-3", "folder//file-7"};
  std::vector<std::string> selected;
  auto subset = decode_archive(packed.bytes, select, [&](DecodedFile file) {
    if (!std::equal(file.bytes.begin(), file.bytes.end(), payload.begin(), payload.end())) {
      std::abort();
    }
    std::lock_guard lock(mutex);
    selected.push_back(file.name);
  });
  std::sort(selected.begin(), selected.end());
  check(selected == std::vector<std::string>{"folder/file-3", "folder/file-7"}, "selected sink");
  check(subset.entries.size() == 2 && subset.original_bytes == 6 &&
            subset.entries[0].name == "folder/file-3" && subset.entries[1].name == "folder/file-7",
        "selected entries in archive order");
  check(subset.entries[0].sha1 == "a9993e364706816aba3e25717850c26c9cd0d89d" &&
            subset.entries[1].sha1 == subset.entries[0].sha1,
        "selected SHA1");
  select.members = {"folder/file-7", "folder/absent"};
  bool delivered = false;
  rejects([&] { decode_archive(packed.bytes, select, [&](DecodedFile) { delivered = true; }); },
          "member not found: folder/absent");
  check(!delivered, "missing member precedes output");
  select.members = {"../escape"};
  rejects([&] { decode_archive(packed.bytes, select); }, "unsafe");
  // The output limit counts the selected files, not the whole archive.
  select.members = {"folder/file-0"};
  select.limits.total_bytes = 3;
  check(decode_archive(packed.bytes, select).entries.size() == 1, "limit covers selection");
  select.members = {"folder/file-0", "folder/file-1"};
  rejects([&] { decode_archive(packed.bytes, select); }, "size limit");
  select.members.clear();
  rejects([&] { decode_archive(packed.bytes, select); }, "size limit");
  check(decode_archive(tiny).entries.size() == 1, "healthy after errors");
  std::printf("archive API: %zu checks passed\n", checks);
}
