#include "../archive.h"
#include "../frames.h"
#include "../parallel.h"
#include <atomic>
#include <cfenv>
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
  check(decode_archive(tiny).entries.size() == 1, "healthy after errors");
  std::printf("archive API: %zu checks passed\n", checks);
}
