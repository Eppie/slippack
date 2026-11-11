#include "../../archive.h"
#include "../../platform.h"
#include "perf.h"
#include <pthread.h>
#include <fstream>
#include <iostream>
#include <filesystem>

using slippack::Bytes;
static Bytes encode(const Bytes &input, const std::string &name) {
  slippack::ArchiveInput f{name, [&] { return input; }};
  slippack::EncodeOptions opts;
  opts.level = 3;
  opts.threads = 1;
  return slippack::encode_archive(std::span(&f, 1), opts).bytes;
}
int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  const std::string mode = argv[2];
  auto named = [](const char *s) { return PerfCounter::Named(s); };
  PerfCounterSet set = CYCLES | INSTRUCTIONS;
  if (mode == "cache") {
    set = set | L1_LOAD_MISS | L1_STORE_MISS | DTLB_MISS | L2_TLB_MISS | named("INST_INT_LD") |
          named("INST_SIMD_LD");
  } else if (mode == "branch") {
    set = set | BRANCHES | BRANCH_MISS | BRANCH_COND_MISS | BRANCH_INDIR_MISS;
  } else if (mode == "stall") {
    set = set | named("MAP_STALL") | named("MAP_STALL_DISPATCH") | MAP_DISPATCH_BUBBLE |
          named("MAP_DISPATCH_BUBBLE_IC") | L1I_CACHE_MISS | CORE_ACTIVE_CYCLE;
  } else if (mode == "traffic") {
    set = set | named("INST_INT_LD") | named("INST_SIMD_LD") | named("INST_INT_ST") |
          named("INST_SIMD_ST") | named("L1D_CACHE_MISS_LD_NONSPEC") | named("MMU_TABLE_WALK_DATA");
  } else {
    return 2;
  }
  std::string error;
  if (!PerfPrimeThread(set, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::ifstream list(argv[1]);
  std::vector<Bytes> inputs, reference;
  std::vector<std::string> names;
  std::string path;
  while (std::getline(list, path)) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
      return 3;
    }
    auto n = f.tellg();
    inputs.emplace_back(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(inputs.back().data()), n);
    if (!f) {
      return 3;
    }
    names.push_back(std::filesystem::path(path).filename().string());
  }
  for (size_t i = 0; i < inputs.size(); ++i) {
    reference.push_back(encode(inputs[i], names[i]));
  }
  for (int rep = 0; rep < std::stoi(argv[3]); ++rep) {
    for (size_t i = 0; i < inputs.size(); ++i) {
      for (int attempt = 0; attempt < 20; ++attempt) {
        Bytes packed;
        auto m = PerfMeasure(set, [&] { packed = encode(inputs[i], names[i]); });
        if (packed != reference[i]) {
          std::cerr << "output changed\n";
          return 4;
        }
        bool accepted = m.valid && m.HasActiveConfigurableCounters() &&
                        m.cpu_before == m.cpu_after && m.cpu_before >= 4 && m.cpu_before < 16;
        std::cout << "{\"mode\":\"" << mode << "\",\"rep\":" << rep << ",\"file\":" << i
                  << ",\"raw_bytes\":" << inputs[i].size() << ",\"packed_bytes\":" << packed.size()
                  << ",\"accepted\":" << (accepted ? "true" : "false")
                  << ",\"measurement\":" << m.ToJson() << "}" << std::endl;
        if (accepted) {
          break;
        }
        if (attempt == 19) {
          std::cerr << "could not get a stable live P-core sample\n";
          return 5;
        }
      }
    }
  }
}
