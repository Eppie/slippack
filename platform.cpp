#include "platform.h"
#include <algorithm>
#include <cstdio>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
#else
#include <malloc.h>
#include <openssl/sha.h>
#endif
namespace slippack {
std::string sha1(View data) {
  uint8_t digest[20];
#if defined(__APPLE__)
  CC_SHA1_CTX context;
  CC_SHA1_Init(&context);
  while (!data.empty()) {
    auto chunk = data.first(std::min<size_t>(data.size(), 1u << 30));
    CC_SHA1_Update(&context, chunk.data(), CC_LONG(chunk.size()));
    data = data.subspan(chunk.size());
  }
  CC_SHA1_Final(digest, &context);
#else
  SHA1(data.data(), data.size(), digest);
#endif
  char hex[41];
  for (unsigned i = 0; i < 20; ++i) {
    std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  return {hex, 40};
}
void release_unused_memory() {
#if defined(__APPLE__)
  malloc_zone_pressure_relief(nullptr, 0);
#else
  malloc_trim(0);
#endif
}
} // namespace slippack
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
