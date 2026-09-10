#include "MiniTest.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

void register_simd_portability_tests()
{
    MiniTest::Case("SIMD portability", [](TestCase &tc) {
        tc.Run("MMI arithmetic matches scalar reference", [](TestCase &t) {
            uint32_t seed = 0x21678u;
            auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
            for (int n = 0; n < 1024; ++n)
            {
                std::array<uint32_t, 4> a, b, got;
                for (int i = 0; i < 4; ++i) { a[i] = next(); b[i] = next(); }
                const auto av = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a.data()));
                const auto bv = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b.data()));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(got.data()), PS2_PADDW(av, bv));
                for (int i = 0; i < 4; ++i) t.Equals(got[i], a[i] + b[i], "PADDW wraps per lane");
                _mm_storeu_si128(reinterpret_cast<__m128i *>(got.data()), PS2_PMAXW(av, bv));
                for (int i = 0; i < 4; ++i)
                    t.Equals(std::bit_cast<int32_t>(got[i]), std::max(std::bit_cast<int32_t>(a[i]), std::bit_cast<int32_t>(b[i])), "PMAXW signed comparison");
                _mm_storeu_si128(reinterpret_cast<__m128i *>(got.data()), PS2_PADDSW(av, bv));
                for (int i = 0; i < 4; ++i)
                {
                    int64_t sum = int64_t(std::bit_cast<int32_t>(a[i])) + std::bit_cast<int32_t>(b[i]);
                    sum = std::clamp(sum, int64_t(INT32_MIN), int64_t(INT32_MAX));
                    t.Equals(std::bit_cast<int32_t>(got[i]), sum, "PADDSW saturates signed overflow");
                }
                _mm_storeu_si128(reinterpret_cast<__m128i *>(got.data()), PS2_PEXTLW(av, bv));
                t.IsTrue(got == std::array<uint32_t, 4>{b[0], a[0], b[1], a[1]}, "PEXTLW lane order");
                _mm_storeu_si128(reinterpret_cast<__m128i *>(got.data()), PS2_PPACW(av, bv));
                t.IsTrue(got == std::array<uint32_t, 4>{b[0], b[2], a[0], a[2]}, "PPACW lane order");
            }
        });
        tc.Run("float blends select by sign bit only", [](TestCase &t) {
            const auto a = _mm_set_ps(4, 3, 2, 1);
            const auto b = _mm_set_ps(8, 7, 6, 5);
            const auto mask = _mm_castsi128_ps(_mm_set_epi32(INT32_MIN, 1, -1, 0));
            std::array<float, 4> got;
            _mm_storeu_ps(got.data(), _mm_blendv_ps(a, b, mask));
            t.IsTrue(got == std::array<float, 4>{1, 6, 3, 8}, "blendv sign-bit semantics");
        });
    });
}
