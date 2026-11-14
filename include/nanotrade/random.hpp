#ifndef NANOTRADE_RANDOM_HPP
#define NANOTRADE_RANDOM_HPP

#include "nanotrade/math.hpp"

#include <cmath>
#include <cstdint>

namespace nanotrade {

// xoshiro256** seeded through splitmix64. The standard library's engines are portable but its distributions are
// not: libc++, libstdc++, and the MSVC STL turn the same bits into different numbers. These distributions are written
// out here, so a seed gives the same market and the same bootstrap on every platform.
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept {
        for (std::uint64_t& word : state_) {
            seed += 0x9e3779b97f4a7c15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
            word = z ^ (z >> 31U);
        }
    }

    std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(state_[1] * 5U, 7) * 9U;
        const std::uint64_t t = state_[1] << 17U;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    // Uniform on [0, 1), from the top 53 bits.
    double uniform() noexcept {
        return static_cast<double>(next() >> 11U) * 0x1.0p-53;
    }

    // Uniform on [0, bound), without modulo bias.
    std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t threshold = (0 - bound) % bound;
        for (;;) {
            const std::uint64_t r = next();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    // Standard normal, by Marsaglia's polar method, which needs no trigonometry.
    double normal() noexcept {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        for (;;) {
            const double u = 2.0 * uniform() - 1.0;
            const double v = 2.0 * uniform() - 1.0;
            const double s = u * u + v * v;
            if (s > 0.0 && s < 1.0) {
                const double factor = std::sqrt(-2.0 * math::log(s) / s);
                spare_ = v * factor;
                has_spare_ = true;
                return u * factor;
            }
        }
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << static_cast<unsigned>(k)) | (x >> static_cast<unsigned>(64 - k));
    }

    std::uint64_t state_[4]{};
    double spare_{0.0};
    bool has_spare_{false};
};

}  // namespace nanotrade

#endif
