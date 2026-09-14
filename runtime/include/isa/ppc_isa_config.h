#pragma once

#include <atomic>
#include <cstdint>

#define MKW_RESTRICT __restrict
#if defined(__aarch64__)
#include "../../third_party/sse2neon/sse2neon.h"

// SSE2NEON deliberately stops at SSE4.2. The translated PPC helpers use the
// two FMA3 operations below to retain a single rounding step for paired-single
// arithmetic; Apple Silicon exposes the same operation directly through NEON.
inline __m128 _mm_fmadd_ps(__m128 a, __m128 b, __m128 c)
{
    return vfmaq_f32(c, a, b);
}

inline __m128 _mm_fmsub_ps(__m128 a, __m128 b, __m128 c)
{
    return vfmaq_f32(vnegq_f32(c), a, b);
}
#else
#include <immintrin.h>
#endif

inline constexpr bool MkwStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#if defined(_MSC_VER)
#define MKW_PPC_FORCE_INLINE __forceinline
#define MKW_PPC_NO_INLINE __declspec(noinline)
#define MKW_PPC_INTERNAL_CALL __regcall
#else
#define MKW_PPC_FORCE_INLINE inline __attribute__((always_inline))
#define MKW_PPC_NO_INLINE __attribute__((noinline))
#define MKW_PPC_INTERNAL_CALL
#endif
#define MKW_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline))
#define MKW_PPC_COLD __attribute__((cold))


using MkwStateFreeResult2 = uint64_t __attribute__((ext_vector_type(2)));
