// One input replay per archive. File I/O and byte comparisons are outside timers.
#ifdef BENCH_SLIPPC
#include "compressor.h"
#else
#include "../../archive.h"
#include "../../platform.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using Buffer = std::vector<uint8_t>;
#ifdef BENCH_SLIPPC
using DecodedBuffer = Buffer;
#else
using DecodedBuffer = slippack::OutputBytes;
#endif
using Clock = std::chrono::steady_clock;

Buffer encode(const Buffer &input, const std::string &name, int level) {
#ifdef BENCH_SLIPPC
  // Advertised support ends at 3.12.0. Do not feed newer layouts to its
  // fixed-size event transforms; report these as outside supported coverage.
  if (input.size() < 32 || input[15] != 0x35) {
    throw std::runtime_error("invalid replay header");
  }
  size_t start = 16 + input[16];
  if (start + 4 >= input.size() || input[start] != 0x36) {
    throw std::runtime_error("missing game start");
  }
  if (std::array<uint8_t, 3>{input[start + 1], input[start + 2], input[start + 3]} >
      std::array<uint8_t, 3>{3, 12, 0}) {
    throw std::runtime_error("outside slippc advertised support (3.12.0)");
  }
  auto codec = std::make_unique<slip::Compressor>(0);
  char *source = reinterpret_cast<char *>(const_cast<uint8_t *>(input.data()));
  if (!codec->loadFromBuff(&source, input.size())) {
    throw std::runtime_error("slippc transform rejected replay");
  }
  char *raw = nullptr;
  unsigned size = codec->saveToBuff(&raw);
  std::unique_ptr<char[]> owner(raw);
  auto packed = slip::compressWithLzma(raw, size, level);
  return Buffer(packed.begin(), packed.end());
#else
  slippack::ArchiveInput file{name, [&] { return input; }};
  slippack::EncodeOptions options;
  options.level = level;
  options.threads = 1;
  return slippack::encode_archive(std::span(&file, 1), options).bytes;
#endif
}

DecodedBuffer decode(const Buffer &input) {
#ifdef BENCH_SLIPPC
  auto raw = slip::decompressWithLzma(input.data(), input.size());
  auto codec = std::make_unique<slip::Compressor>(0);
  char *source = raw.data();
  if (!codec->loadFromBuff(&source, raw.size())) {
    throw std::runtime_error("slippc inverse transform failed");
  }
  char *out = nullptr;
  auto size = codec->saveToBuff(&out);
  std::unique_ptr<char[]> owner(out);
  return Buffer(out, out + size);
#else
  DecodedBuffer output;
  slippack::decode_archive(input, {},
                           [&](slippack::DecodedFile file) { output = std::move(file.bytes); });
  return output;
#endif
}

template <class Fn> void workers(size_t count, unsigned threads, Fn fn) {
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (;;) {
      auto i = next.fetch_add(1);
      if (i >= count) {
        return;
      }
      fn(i);
    }
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < threads; ++i) {
    pool.emplace_back(work);
  }
  work();
  for (auto &thread : pool) {
    thread.join();
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
#ifdef BENCH_SLIPPC
  // Initialize upstream's unsynchronized lazy static before parallel calls.
  slip::Compressor::readLegacyGeckoCodes();
#endif
  std::ifstream manifest(argv[1]);
  std::vector<Buffer> inputs;
  std::vector<std::string> names;
  std::string path;
  while (std::getline(manifest, path)) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("cannot read input");
    }
    inputs.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    names.push_back(std::filesystem::path(path).filename().string());
  }
  std::cout << "{\"ready\":true,\"files\":" << inputs.size() << "}" << std::endl;
  int level;
  unsigned threads;
  while (std::cin >> level >> threads) {
    if (!threads || threads > 256) {
      return 2;
    }
    std::vector<Buffer> compressed(inputs.size());
    std::vector<DecodedBuffer> decoded(inputs.size());
    std::vector<std::string> errors(inputs.size());
    auto start = Clock::now();
    workers(inputs.size(), threads, [&](size_t i) {
      try {
        compressed[i] = encode(inputs[i], names[i], level);
      } catch (const std::exception &error) {
        errors[i] = error.what();
      }
    });
    double enc = std::chrono::duration<double>(Clock::now() - start).count();
    start = Clock::now();
    workers(inputs.size(), threads, [&](size_t i) {
      if (errors[i].empty()) {
        try {
          decoded[i] = decode(compressed[i]);
        } catch (const std::exception &error) {
          errors[i] = error.what();
        }
      }
    });
    double dec = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << std::setprecision(10) << "{\"encode_seconds\":" << enc
              << ",\"decode_seconds\":" << dec << ",\"files\":[";
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (errors[i].empty() &&
          !std::equal(inputs[i].begin(), inputs[i].end(), decoded[i].begin(), decoded[i].end())) {
        errors[i] = "round-trip bytes differ";
      }
      if (i) {
        std::cout << ',';
      }
      std::cout << "{\"bytes\":" << compressed[i].size();
#ifndef BENCH_SLIPPC
      std::cout << ",\"packed_sha1\":" << std::quoted(slippack::sha1(compressed[i]));
#endif
      std::cout << ",\"error\":" << std::quoted(errors[i]) << '}';
    }
    std::cout << "]}" << std::endl;
  }
}
