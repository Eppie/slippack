#pragma once

// Compile only instructions enabled for this x86 build, retaining scalar
// fallbacks for unavailable extensions. ARM keeps its existing NEON paths;
// CUSTOM_SCALAR disables both architectures' SIMD for differential testing.
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SSE2__) &&                             \
    !defined(SLIPPACK_CUSTOM_SCALAR)
#include <immintrin.h>
#define SLIPPACK_SSE2 1
#else
#define SLIPPACK_SSE2 0
#endif

#if SLIPPACK_SSE2 && defined(__AVX512BW__)
#define SLIPPACK_AVX512BW 1
#else
#define SLIPPACK_AVX512BW 0
#endif

#if SLIPPACK_AVX512BW && defined(__AVX512VBMI2__)
#define SLIPPACK_AVX512VBMI2 1
#else
#define SLIPPACK_AVX512VBMI2 0
#endif
