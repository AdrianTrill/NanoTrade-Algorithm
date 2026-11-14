#ifndef NANOTRADE_MATH_HPP
#define NANOTRADE_MATH_HPP

// Elementary functions that give the same bits everywhere. Results end up in golden files that must match byte for
// byte on Linux, macOS, Windows, and in WebAssembly, but each platform's libm rounds exp, log, and pow differently in
// the last bit. These use only IEEE arithmetic, built without fused multiply-adds (-ffp-contract=off), so every
// platform rounds every step alike. exp and log follow fdlibm and are accurate to about one unit in the last place.

namespace nanotrade::math {

double exp(double x) noexcept;
double log(double x) noexcept;
// IEEE 754's special cases, and otherwise exp(exponent * log(base)): within 2 + 3 |exponent * log(base)| ulps.
double pow(double base, double exponent) noexcept;

double normal_cdf(double x) noexcept;       // the standard normal distribution function, to about 1e-15
double normal_quantile(double p) noexcept;  // its inverse, for 0 < p < 1

inline constexpr double euler_gamma = 0.57721566490153286061;
inline constexpr double e = 2.71828182845904523536;

}  // namespace nanotrade::math

#endif
