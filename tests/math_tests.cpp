// nanotrade::math against the standard library and against values worked to 60 digits with mpmath: exp, log, and pow
// over dense sweeps from the subnormals to the overflow threshold, in units in the last place; their special values,
// with pow's IEEE 754 cases for zeros, negative bases, and the infinities; normal_cdf against erfc, at known points,
// at its ends, and for monotonicity; normal_quantile at known points, down both tails to the smallest double, under
// symmetry, through round trips, and for monotonicity; and the two constants.

#include "check.hpp"

#include "nanotrade/math.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>

namespace {

namespace math = nanotrade::math;

constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double largest = std::numeric_limits<double>::max();
constexpr double smallest_normal = std::numeric_limits<double>::min();
constexpr double smallest = std::numeric_limits<double>::denorm_min();

// ---- Helpers ----

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// The doubles numbered in order: neighbours differ by 1, both zeros are 0, and each infinity comes right after the
// largest finite double of its sign.
std::int64_t ordinal(double x) {
    const auto bits = std::bit_cast<std::uint64_t>(x);
    const auto magnitude = static_cast<std::int64_t>(bits & 0x7FFF'FFFF'FFFF'FFFFULL);
    return (bits >> 63U) != 0 ? -magnitude : magnitude;
}

double from_ordinal(std::int64_t n) {
    const auto magnitude = static_cast<std::uint64_t>(n < 0 ? -n : n);
    return std::bit_cast<double>(n < 0 ? magnitude | 0x8000'0000'0000'0000ULL : magnitude);
}

// How many doubles apart a and b are: 0 when they are equal, 1 when they are neighbours. NaN is infinitely far from
// everything but NaN.
double ulps_apart(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b) ? 0.0 : inf;
    }
    const std::int64_t i = ordinal(a);
    const std::int64_t j = ordinal(b);
    // Unsigned, as the distance from the most negative double to the most positive one overflows 63 bits.
    const std::uint64_t distance = i >= j ? static_cast<std::uint64_t>(i) - static_cast<std::uint64_t>(j)
                                          : static_cast<std::uint64_t>(j) - static_cast<std::uint64_t>(i);
    return static_cast<double>(distance);
}

// The gap from |x| to the next double up.
double ulp(double x) {
    const double magnitude = std::fabs(x);
    return std::nextafter(magnitude, inf) - magnitude;
}

// The same double: equal and with the same sign, zeros included, or both NaN.
bool identical(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || (a == b && std::signbit(a) == std::signbit(b));
}

// The standard normal density.
double density(double x) {
    return std::exp(-x * x / 2.0) / 2.5066282746310002;
}

// Phi(x) = erfc(-x / sqrt(2)) / 2. Rounding x / sqrt(2) would cost up to x^2 ulps out in the tail, where erfc(z) falls
// by a factor e^(2 z dz) as z grows by dz, so the rounding error is recovered with a fused multiply-add and added back
// to first order: erfc(z + dz) = erfc(z) - 2 / sqrt(pi) e^(-z^2) dz.
double reference_cdf(double x) {
    constexpr double root_half = 0x1.6a09e667f3bcdp-1;        // 1 / sqrt(2), rounded
    constexpr double root_half_low = -4.833646656726457e-17;  // 1 / sqrt(2) - root_half
    constexpr double two_over_root_pi = 1.1283791670955126;
    const double z = -x * root_half;
    const double dz = std::fma(-x, root_half, -z) - x * root_half_low;
    return 0.5 * (std::erfc(z) - two_over_root_pi * std::exp(-z * z) * dz);
}

// The largest error a sweep has seen, and where.
struct Worst {
    double error = 0.0;
    double x = 0.0;
    double y = 0.0;

    void add(double value, double at_x, double at_y = 0.0) {
        if (!(value <= error)) {
            error = std::isnan(value) ? inf : value;
            x = at_x;
            y = at_y;
        }
    }
};

void expect_at_most(const Worst& worst, double bound, const std::string& what, const char* name = "x") {
    if (!(worst.error <= bound)) {
        throw check::Failure(what + ": " + text_of(worst.error) + " at " + name + " = " + text_of(worst.x) +
                             ", over the bound of " + text_of(bound));
    }
}

// A quantile worked to 60 digits, for p as the double it is: 0.975 is 0.97499999999999997780.
struct Quantile {
    double p;
    double x;
};

// ---- exp, log, and pow against the standard library ----

// fdlibm's exp is faithful, within an ulp of the true value, and so is a good libm's std::exp (glibc's and Apple's
// are): each gives one of the two doubles either side of the true value, so the two are never more than 1 apart.
// Subnormal results keep that, as ldexp rounds them once. Measured worst: 1.
void exp_against_std() {
    Worst worst;
    const auto at = [&](double x) {
        worst.add(ulps_apart(math::exp(x), std::exp(x)), x);
    };
    // Evenly over the whole range, through the subnormal results below -708.4.
    constexpr int steps = 600000;
    for (int i = 0; i <= steps; ++i) {
        at(-745.2 + 1455.0 * i / steps);
    }
    // Every binade from the smallest double up to 745, of both signs: exp near 1.
    const std::int64_t top = ordinal(745.2);
    constexpr std::int64_t samples = 300000;
    for (std::int64_t i = 0; i <= samples; ++i) {
        at(from_ordinal(top / samples * i));
        at(-from_ordinal(top / samples * i));
    }
    // The doubles next to the overflow threshold, ln(largest) = 709.782712893384, and next to the underflow threshold,
    // where exp is half the smallest double.
    for (std::int64_t i = -20000; i <= 20; ++i) {
        at(from_ordinal(ordinal(709.782712893384) + i));
    }
    for (std::int64_t i = -20000; i <= 20000; ++i) {
        at(from_ordinal(ordinal(-745.13321910194110842) + i));
    }
    expect_at_most(worst, 1.0, "exp against std::exp, in ulps");
}

// Like exp: fdlibm's log and every std::log are faithful, so they are never more than 1 apart. Measured worst: 1.
void log_against_std() {
    Worst worst;
    const auto at = [&](double x) {
        worst.add(ulps_apart(math::log(x), std::log(x)), x);
    };
    // Every binade of the positive doubles, from the subnormals to the largest.
    const std::int64_t top = ordinal(largest);
    constexpr std::int64_t samples = 600000;
    for (std::int64_t i = 1; i <= samples; ++i) {
        at(from_ordinal(top / samples * i));
    }
    // The 400,001 doubles around 1, where log is near 0.
    for (std::int64_t i = -200000; i <= 200000; ++i) {
        at(from_ordinal(ordinal(1.0) + i));
    }
    // Evenly over [0.5, 2] and [1, 10^6].
    constexpr int steps = 300000;
    for (int i = 0; i <= steps; ++i) {
        at(0.5 + 1.5 * i / steps);
        at(1.0 + 999999.0 * i / steps);
    }
    // Either side of sqrt(2) / 2 times powers of two, where the reduction to 2^k (1 + f) moves on to the next k, down
    // to the subnormals.
    for (const int k : {0, 1, -1000, 1000, -1040}) {
        const double edge = std::ldexp(0x1.6a09e667f3bcdp-1, k);
        for (std::int64_t i = -2000; i <= 2000; ++i) {
            at(from_ordinal(ordinal(edge) + i));
        }
    }
    // The powers of two, whose logarithms are multiples of ln 2.
    for (int k = -1074; k <= 1023; ++k) {
        at(std::ldexp(1.0, k));
    }
    expect_at_most(worst, 1.0, "log against std::log, in ulps");
}

// For x > 0, pow(x, y) is exp(y log x), and loses more than its parts: log's ulp and the product's rounding put y log x
// off by up to 3 * 2^-53 of itself, which exp turns into the same relative error, up to 3 |y ln x| ulps of the result,
// as an ulp is at least 2^-53 of a value. With an ulp each for exp and std::pow, the bound is 2 + 3 |y ln x|: as good
// as exp and log near 1 (within an ulp, measured, wherever |y ln x| <= 1), but about 1.6 |y ln x| ulps far from it.
// The worst measured is 1105 ulps, at 10^308.25; and pow(2, 1024) comes out finite, 1.7976931348622732e308, where
// std::pow overflows. Negative bases are their magnitudes' powers (see pow_negative_bases).
void pow_against_std() {
    Worst worst;  // in units of the bound
    const auto at = [&](double x, double y) {
        const double bound = 2.0 + 3.0 * std::fabs(y * std::log(x));
        worst.add(ulps_apart(math::pow(x, y), std::pow(x, y)) / bound, x, y);
    };
    for (const double x :
         {smallest, 1e-310, smallest_normal, 1e-300,        1e-100,        1e-10,    1e-3,   0.1, 0.25, 0.5, 0.75,
          0.9,      0.99,   0.999999,        1.0 - 0x1p-53, 1.0 + 0x1p-52, 1.000001, 1.01,   1.1, 1.5,  2.0, math::e,
          3.0,      10.0,   100.0,           1e10,          1e100,         1e300,    largest}) {
        for (const double y : {1e300, 1e10, 1e5, 1000.0, 100.0, 10.0, 3.0, 2.0, 1.0, 0.5, 1.0 / 3.0, 1e-10, 1e-300}) {
            at(x, y);
            at(x, -y);
        }
    }
    // Results all over the range: a base in each of 1001 binades of e, with exponents that aim y ln x from below the
    // underflow threshold to past the overflow one.
    constexpr int bases = 1000;
    constexpr int targets = 300;
    for (int i = 0; i <= bases; ++i) {
        const double x = std::exp(-700.0 + 1400.0 * i / bases);
        for (int j = 0; j <= targets; ++j) {
            const double y = (-746.0 + 1456.5 * j / targets) / std::log(x);
            if (std::isfinite(y)) {
                at(x, y);
            }
        }
    }
    // Consecutive exponents up to overflow and down to underflow, and bases next to 1 raised to huge powers.
    constexpr int run = 10000;
    for (int j = 0; j <= run; ++j) {
        const double f = static_cast<double>(j) / run;
        at(2.0, 1000.0 + 24.5 * f);
        at(10.0, 300.0 + 8.3 * f);
        at(0.5, 1000.0 + 76.0 * f);
        at(10.0, -300.0 - 24.0 * f);
        at(1.0 + 1e-6 * f, 1e8);
    }
    if (!(worst.error <= 1.0)) {
        const double ulps = ulps_apart(math::pow(worst.x, worst.y), std::pow(worst.x, worst.y));
        throw check::Failure("pow(" + text_of(worst.x) + ", " + text_of(worst.y) + ") is " + text_of(ulps) +
                             " ulps from std::pow, over the bound of 2 + 3 |y ln x| = " + text_of(ulps / worst.error));
    }
}

// ---- Special values ----

void exp_special_values() {
    check::expect_equal(math::exp(0.0), 1.0, "exp(0)");
    check::expect_equal(math::exp(-0.0), 1.0, "exp(-0)");
    check::expect(math::exp(-inf) == 0.0 && !std::signbit(math::exp(-inf)), "exp(-inf) is +0");
    check::expect_equal(math::exp(inf), inf, "exp(inf)");
    check::expect(std::isnan(math::exp(nan)), "exp(NaN) is NaN");
    // ln(largest) rounds down to 709.782712893384, so its exp is finite; the next double up overflows.
    check::expect_equal(math::exp(709.782712893384), 1.7976931348622732e308, "the largest exp");
    check::expect_equal(math::exp(std::nextafter(709.782712893384, inf)), inf, "overflow");
    check::expect_equal(math::exp(710.0), inf, "exp(710)");
    check::expect_equal(math::exp(largest), inf, "exp(largest)");
    // exp(-745.13321910194110842) is just over half the smallest double, and rounds up to it; further out, +0.
    check::expect_equal(math::exp(-745.13321910194110842), smallest, "the smallest exp");
    check::expect_equal(math::exp(-745.0), smallest, "exp(-745)");
    check::expect(math::exp(-745.2) == 0.0 && !std::signbit(math::exp(-745.2)), "exp(-745.2) underflows to +0");
    check::expect_equal(math::exp(-largest), 0.0, "exp(-largest)");
    // Near 0, exp(x) = 1 + x + x^2 / 2: 1 + 2^-53 ties to 1, and 2^-52 is 1 + 2^-52 to the nearest double.
    check::expect_equal(math::exp(smallest), 1.0, "exp(smallest)");
    check::expect_equal(math::exp(-smallest), 1.0, "exp(-smallest)");
    check::expect_equal(math::exp(0x1p-53), 1.0, "exp(2^-53)");
    check::expect_equal(math::exp(0x1p-52), 1.0 + 0x1p-52, "exp(2^-52)");
}

void log_special_values() {
    check::expect(math::log(1.0) == 0.0 && !std::signbit(math::log(1.0)), "log(1) is +0");
    check::expect_equal(math::log(0.0), -inf, "log(0)");
    check::expect_equal(math::log(-0.0), -inf, "log(-0)");
    check::expect_equal(math::log(inf), inf, "log(inf)");
    for (const double x : {-smallest, -1e-300, -0.5, -1.0, -largest, -inf, nan}) {
        check::expect(std::isnan(math::log(x)), "log(" + text_of(x) + ") is NaN");
    }
    // Correctly rounded, against values worked to 60 digits.
    check::expect_equal(math::log(2.0), 0x1.62e42fefa39efp-1, "log(2)");
    check::expect_equal(math::log(smallest), -0x1.74385446d71c3p+9, "log(smallest), -744.44007192138122");
    check::expect_equal(math::log(smallest_normal), -708.3964185322641, "log(smallest normal)");
    check::expect_equal(math::log(largest), 709.782712893384, "log(largest)");
    check::expect_equal(math::log(math::e), 1.0, "log(e)");
    // Next to 1, log(1 + d) = d - d^2 / 2: just under 2^-52 above 1, and -2^-53 below it.
    check::expect_equal(math::log(1.0 + 0x1p-52), std::nextafter(0x1p-52, 0.0), "log(1 + 2^-52)");
    check::expect_equal(math::log(1.0 - 0x1p-53), -0x1p-53, "log(1 - 2^-53)");
}

// IEEE 754's special cases, which std::pow follows too. x^0 and 1^y are 1 for every x and y, even NaN. A zero or a
// negative base to an odd whole power keeps its sign, and to an even one loses it; a negative base to a fractional
// power is NaN; and the infinities go where the magnitudes say.
void pow_special_values() {
    for (const double x : {0.0, -0.0, 1.0, 2.0, -2.0, smallest, largest, inf, -inf, nan}) {
        check::expect_equal(math::pow(x, 0.0), 1.0, text_of(x) + "^0");
        check::expect_equal(math::pow(x, -0.0), 1.0, text_of(x) + "^-0");
    }
    for (const double y : {1.0, -1.0, 0.5, 1e300, -1e300, inf, -inf, nan}) {
        check::expect_equal(math::pow(1.0, y), 1.0, "1^" + text_of(y));
    }
    struct Case {
        double x;
        double y;
        double power;
    };
    const Case cases[] = {
        // Zero: +-0 to an odd power, +-inf to a negative odd one; otherwise +0 for y > 0, +inf for y < 0.
        {0.0, 3.0, 0.0},
        {-0.0, 3.0, -0.0},
        {-0.0, 1.0, -0.0},
        {-0.0, 9007199254740991.0, -0.0},  // 2^53 - 1, the largest odd double; every double from 2^53 up is even
        {0.0, -3.0, inf},
        {-0.0, -3.0, -inf},
        {-0.0, -1.0, -inf},
        {-0.0, 2.0, 0.0},
        {-0.0, -2.0, inf},
        {-0.0, 0.5, 0.0},
        {-0.0, -0.5, inf},
        {-0.0, 1e300, 0.0},
        {-0.0, -1e300, inf},
        {0.0, smallest, 0.0},
        {0.0, inf, 0.0},
        {-0.0, inf, 0.0},
        {0.0, -inf, inf},
        {-0.0, -inf, inf},
        // Negative bases: whole powers are real, negative when odd; fractional ones are NaN.
        {-2.0, 2.0, 4.0},
        {-2.0, -1.0, -0.5},
        {-1.0, 3.0, -1.0},
        {-1.0, 4.0, 1.0},
        {-1.0, -9007199254740991.0, -1.0},
        {-1.0, 1e300, 1.0},
        {-2.0, 1026.0, inf},
        {-2.0, 1025.0, -inf},
        {-2.0, -1074.0, smallest},
        {-2.0, -1075.0, -0.0},
        {-smallest, 2.0, 0.0},
        {-smallest, 3.0, -0.0},
        {-largest, 2.0, inf},
        {-largest, 3.0, -inf},
        {-2.0, 0.5, nan},
        {-8.0, 1.0 / 3.0, nan},
        {-1.0, 0.5, nan},
        {-2.0, -2.5, nan},
        {-2.0, 1e-300, nan},
        {-smallest, 0.5, nan},
        {-largest, 1.5, nan},
        // The infinities.
        {inf, 2.0, inf},
        {inf, 0.5, inf},
        {inf, -2.0, 0.0},
        {inf, -0.5, 0.0},
        {inf, inf, inf},
        {inf, -inf, 0.0},
        {-inf, 3.0, -inf},
        {-inf, 2.0, inf},
        {-inf, -3.0, -0.0},
        {-inf, -2.0, 0.0},
        {2.0, inf, inf},
        {0.5, inf, 0.0},
        {2.0, -inf, 0.0},
        {0.5, -inf, inf},
        {largest, inf, inf},
        {smallest, inf, 0.0},
        // NaN in, NaN out, but for x^0 and 1^y.
        {nan, 1.0, nan},
        {nan, -2.0, nan},
        {nan, inf, nan},
        {2.0, nan, nan},
        {0.0, nan, nan},
        {-1.0, nan, nan},
        {inf, nan, nan},
        // Overflow and underflow.
        {10.0, 400.0, inf},
        {largest, 2.0, inf},
        {smallest, -1.0, inf},
        {10.0, -400.0, 0.0},
        {smallest, 2.0, 0.0},
        {0.5, 1076.0, 0.0},
        {2.0, -1074.0, smallest},
        // Exact, as they happen to round.
        {2.0, 10.0, 1024.0},
        {4.0, 0.5, 2.0},
    };
    for (const Case& known : cases) {
        const double power = math::pow(known.x, known.y);
        check::expect(identical(power, known.power),
                      "(" + text_of(known.x) + ")^" + text_of(known.y) + " is " + text_of(power) + ", not " +
                          text_of(known.power));
    }
    // exp(y log x) rounds twice, so not every exact answer comes out exact: 10^2 is 3 ulps over 100, and (-2)^3, as
    // -(2^3), is 2 ulps short of -8. Both are within pow's bound (see pow_against_std).
    check::expect(ulps_apart(math::pow(10.0, 2.0), 100.0) <= 4.0, "10^2 is within the bound of 100");
    check::expect(ulps_apart(math::pow(-2.0, 3.0), -8.0) <= 4.0, "(-2)^3 is within the bound of -8");
}

// A negative base to a whole power is its magnitude's power, negated when the power is odd, so it is exactly as
// accurate as a positive base; to a fractional power, NaN. For bases across the range, every whole exponent from -1100
// to 1100, which carries the results from 0 to infinity, and the half-way exponents between them; then the largest
// odd double, the even ones beyond it, and the doubles next to 3.
void pow_negative_bases() {
    Worst worst;  // ulps from std::pow, in units of pow's bound
    for (const double x :
         {smallest, 1e-300, 1e-10, 0.1, 0.5, 0.9, 0.999999, 1.000001, 1.5, 2.0, 3.0, 10.0, 1e10, 1e300, largest}) {
        for (int n = -1100; n <= 1100; ++n) {
            const double y = n;
            const double power = math::pow(-x, y);
            const double magnitude = math::pow(x, y);
            if (!identical(power, n % 2 == 0 ? magnitude : -magnitude)) {
                throw check::Failure("(" + text_of(-x) + ")^" + std::to_string(n) + " is " + text_of(power) +
                                     ", where " + text_of(x) + "^" + std::to_string(n) + " is " + text_of(magnitude));
            }
            worst.add(ulps_apart(power, std::pow(-x, y)) / (2.0 + 3.0 * std::fabs(y * std::log(x))), -x, y);
            if (!std::isnan(math::pow(-x, y + 0.5))) {
                throw check::Failure("(" + text_of(-x) + ")^" + text_of(y + 0.5) + " isn't NaN");
            }
        }
        for (const double y : {9007199254740991.0, 9007199254740992.0, 1e300, largest}) {
            check::expect(identical(math::pow(-x, y), y == 9007199254740991.0 ? -math::pow(x, y) : math::pow(x, y)),
                          "(" + text_of(-x) + ")^" + text_of(y));
            check::expect(identical(math::pow(-x, -y), y == 9007199254740991.0 ? -math::pow(x, -y) : math::pow(x, -y)),
                          "(" + text_of(-x) + ")^" + text_of(-y));
        }
        for (const double y : {std::nextafter(3.0, 4.0), std::nextafter(3.0, 2.0), std::nextafter(-3.0, -4.0)}) {
            check::expect(std::isnan(math::pow(-x, y)), "(" + text_of(-x) + ")^" + text_of(y) + " is NaN");
        }
    }
    if (!(worst.error <= 1.0)) {
        throw check::Failure("(" + text_of(worst.x) + ")^" + text_of(worst.y) + " is " +
                             text_of(ulps_apart(math::pow(worst.x, worst.y), std::pow(worst.x, worst.y))) +
                             " ulps from std::pow, over pow's bound");
    }
}

// IEEE 754's cases for an infinite exponent, which go by the base's magnitude whatever its sign: 1 for |x| = 1, and
// otherwise +0 or +inf as |x| < 1 or |x| > 1 and the exponent's sign decide, zeros and infinities among the bases;
// and for an infinite base to a finite power, +inf or +0 as the power's sign says, but an odd whole power of -inf
// keeps its sign. (Before, a negative base to an infinite power, and -inf to a fractional one, were NaN.)
void pow_infinite_cases() {
    const auto expect_power = [](double x, double y, double power) {
        check::expect(
            identical(math::pow(x, y), power),
            "(" + text_of(x) + ")^" + text_of(y) + " is " + text_of(math::pow(x, y)) + ", not " + text_of(power));
    };
    for (const double x : {-0.5, -smallest, -0.0, 0.0, smallest, 0.5}) {
        expect_power(x, inf, 0.0);
        expect_power(x, -inf, inf);
    }
    for (const double x : {-inf, -largest, -2.0, -1.0 - 0x1p-52, 1.0 + 0x1p-52, 2.0, largest, inf}) {
        expect_power(x, inf, inf);
        expect_power(x, -inf, 0.0);
    }
    for (const double x : {-1.0, 1.0}) {
        expect_power(x, inf, 1.0);
        expect_power(x, -inf, 1.0);
    }
    struct Case {
        double x;
        double y;
        double power;
    };
    const Case cases[] = {
        {-inf, 0.5, inf},
        {-inf, 1.0 / 3.0, inf},
        {-inf, 2.0, inf},
        {-inf, 1e300, inf},
        {-inf, smallest, inf},
        {-inf, -0.5, 0.0},
        {-inf, -2.0, 0.0},
        {-inf, -1e300, 0.0},
        {-inf, 1.0, -inf},
        {-inf, 3.0, -inf},
        {-inf, 9007199254740991.0, -inf},
        {-inf, -1.0, -0.0},
        {-inf, -3.0, -0.0},
        {inf, 0.5, inf},
        {inf, 3.0, inf},
        {inf, -0.5, 0.0},
        {inf, -3.0, 0.0},
    };
    for (const Case& known : cases) {
        expect_power(known.x, known.y, known.power);
    }
}

// ---- The normal distribution ----

// Hart's rational function, used below |x| = 7.07, and West's continued fraction, used above, are fitted for absolute
// error, which the header puts at about 1e-15; measured, it is at most 2.2e-16, an ulp of 1. For x >= -1, where
// Phi >= 0.16, that is a few ulps (7 measured), and std::erfc, the reference, may be a few ulps out itself on some
// platforms, so 16 are allowed. In the lower tail the same absolute error is large relative to Phi: 2.5e-13 by x = -4,
// 1.3e-9 at -6.4, and 8.9e-9 (8e7 ulps) at -7.8, just past the switch to the continued fraction, falling back to
// 1.7e-13 by -37. The header promises no better, but normal_quantile inherits it (see normal_quantile_lower_tail).
void normal_cdf_against_erfc() {
    Worst absolute;
    Worst center;  // in ulps, for x >= -1
    Worst tail;    // relative, for x < -1
    constexpr int steps = 740000;
    for (int i = 0; i <= steps; ++i) {
        const double x = -37.0 + 74.0 * i / steps;
        const double value = math::normal_cdf(x);
        const double expected = reference_cdf(x);
        absolute.add(std::fabs(value - expected), x);
        if (x >= -1.0) {
            center.add(ulps_apart(value, expected), x);
        } else {
            tail.add(std::fabs(value / expected - 1.0), x);
        }
    }
    expect_at_most(absolute, 1e-15, "the absolute error");
    expect_at_most(center, 16.0, "the error in ulps for x >= -1");
    expect_at_most(tail, 1e-8, "the relative error for x < -1");
}

// Phi(x) is 1 - t where Phi(-x) is t, and 1 - t + t rounds to exactly 1, so the halves add up exactly.
void normal_cdf_symmetry() {
    constexpr int steps = 400000;
    for (int i = 0; i <= steps; ++i) {
        const double x = 40.0 * i / steps;
        if (math::normal_cdf(x) + math::normal_cdf(-x) != 1.0) {
            throw check::Failure("Phi(" + text_of(x) + ") + Phi(" + text_of(-x) + ") is " +
                                 text_of(math::normal_cdf(x) + math::normal_cdf(-x)));
        }
    }
}

void normal_cdf_known_values() {
    check::expect_equal(math::normal_cdf(0.0), 0.5, "Phi(0)");
    check::expect_equal(math::normal_cdf(-0.0), 0.5, "Phi(-0)");
    // Worked to 60 digits: within an ulp of 1 near the middle, as in normal_cdf_against_erfc, and within its relative
    // error out in the tail.
    const Quantile middle[] = {{0.024997895148220436213, -1.96},
                               {0.15865525393145705141, -1.0},
                               {0.69146246127401310364, 0.5},
                               {0.84134474606854294859, 1.0},
                               {0.97500210485177956379, 1.96}};
    for (const Quantile& known : middle) {
        check::expect_near(math::normal_cdf(known.x), known.p, 0x1p-52, "Phi(" + text_of(known.x) + ")");
    }
    const Quantile tail[] = {{7.619853024160526066e-24, -10.0},
                             {2.7536241186062336951e-89, -20.0},
                             {5.7255712225245768227e-300, -37.0}};
    for (const Quantile& known : tail) {
        check::expect_near(math::normal_cdf(known.x), known.p, 1e-8 * known.p, "Phi(" + text_of(known.x) + ")");
    }
    // The ends. Beyond |x| = 37 the tail is taken as 0, as in Hart's algorithm: within 1e-15 of Phi(-37.5) = 4.6e-308,
    // and normal_quantile no longer leans on it there (see normal_quantile_far_tail). 1 - Phi(9) = 1.1e-19 rounds away.
    check::expect_equal(math::normal_cdf(-inf), 0.0, "Phi(-inf)");
    check::expect_equal(math::normal_cdf(-37.5), 0.0, "Phi(-37.5)");
    check::expect(math::normal_cdf(-37.0) > 0.0, "Phi(-37) is not 0");
    check::expect_equal(math::normal_cdf(9.0), 1.0, "Phi(9)");
    check::expect_equal(math::normal_cdf(37.5), 1.0, "Phi(37.5)");
    check::expect_equal(math::normal_cdf(inf), 1.0, "Phi(inf)");
    check::expect(std::isnan(math::normal_cdf(nan)), "Phi(NaN) is NaN");
}

// Non-decreasing on a grid over [-38, 38], and ulp by ulp across the switch to the continued fraction at |x| = 7.07,
// the cutoff at -37, and 0. Single ulps elsewhere are noise: near x = -1, where an ulp of x moves Phi by about an ulp,
// rounding steps Phi back by up to 3 ulps now and then (measured), so 4 are allowed there.
void normal_cdf_is_monotonic() {
    double previous = 0.0;
    constexpr int steps = 1520000;
    for (int i = 0; i <= steps; ++i) {
        const double x = -38.0 + 76.0 * i / steps;
        const double value = math::normal_cdf(x);
        if (value < previous) {
            throw check::Failure("Phi(" + text_of(x) + ") = " + text_of(value) + " is below the step before's " +
                                 text_of(previous));
        }
        previous = value;
    }
    for (const double edge : {-37.0, -7.07106781186547, 0.0, 7.07106781186547, -1.0}) {
        double x = from_ordinal(ordinal(edge) - 20000);
        double before = math::normal_cdf(x);
        for (int i = 0; i < 40000; ++i) {
            x = from_ordinal(ordinal(x) + 1);
            const double value = math::normal_cdf(x);
            const double allowed = edge == -1.0 ? 4.0 : 0.0;
            if (value < before && ulps_apart(value, before) > allowed) {
                throw check::Failure("Phi(" + text_of(x) + ") = " + text_of(value) + " is " +
                                     text_of(ulps_apart(value, before)) + " ulps below Phi a double before");
            }
            before = value;
        }
    }
}

// p near 1 pins x down only to ulp(p) / phi(x): p's neighbours have quantiles that far apart, and normal_cdf(x) - p,
// which Halley's step drives to 0, resolves no finer. And a few ulps of x for the arithmetic.
double quantile_tolerance(double p, double x) {
    return 4.0 * ulp(x) + ulp(p) / density(x);
}

void normal_quantile_known_values() {
    check::expect(math::normal_quantile(0.5) == 0.0, "the median is 0");
    check::expect_near(math::normal_quantile(0.975), 1.959963984540054, 1e-15, "the 97.5th percentile");
    check::expect_near(math::normal_quantile(0.025), -1.959963984540054, 1e-15, "the 2.5th percentile");
    // Either side of Acklam's switch between his central and tail approximations, at 0.02425 and 0.97575, and up
    // the upper tail to the largest double below 1.
    const Quantile quantiles[] = {{0.02425, -1.972961051311884837603},
                                  {0.025, -1.95996398454005421178},
                                  {0.1, -1.281551565544600435335},
                                  {0.25, -0.6744897501960817432022},
                                  {0.75, 0.6744897501960817432022},
                                  {0.9, 1.281551565544600593487},
                                  {0.95, 1.644853626951472284276},
                                  {0.975, 1.959963984540053855604},
                                  {0.97575, 1.972961051311884959401},
                                  {0.99, 2.326347874040840767637},
                                  {0.999, 3.090232306167813277758},
                                  {1.0 - 0x1p-20, 4.763001034267813956989},
                                  {1.0 - 0x1p-40, 7.047700256664408725351},
                                  {1.0 - 0x1p-53, 8.209536151601386855631}};
    for (const Quantile& known : quantiles) {
        check::expect_near(math::normal_quantile(known.p),
                           known.x,
                           quantile_tolerance(known.p, known.x),
                           "the quantile of " + text_of(known.p));
    }
}

void normal_quantile_ends() {
    check::expect_equal(math::normal_quantile(0.0), -inf, "the quantile of 0");
    check::expect_equal(math::normal_quantile(-0.0), -inf, "the quantile of -0");
    check::expect_equal(math::normal_quantile(1.0), inf, "the quantile of 1");
    for (const double p : {-smallest, -0.5, -1.0, std::nextafter(1.0, 2.0), 2.0, -inf, inf, nan}) {
        check::expect(std::isnan(math::normal_quantile(p)), "the quantile of " + text_of(p) + " is NaN");
    }
}

// For p = k / 2^20, 1 - p is exact, and Q(1 - p) = -Q(p) to within what p near 1 resolves, 2^-53 / phi(x) (see
// quantile_tolerance); twice that is allowed, and 0.97 of it measured.
void normal_quantile_symmetry() {
    const auto at = [](double p) {
        const double low = math::normal_quantile(p);
        const double high = math::normal_quantile(1.0 - p);
        if (!(std::fabs(low + high) <= 0x1p-52 / density(low))) {
            throw check::Failure("Q(" + text_of(p) + ") = " + text_of(low) + " but Q(1 - p) = " + text_of(high));
        }
    };
    for (int k = 1; k < (1 << 19); ++k) {
        at(k * 0x1p-20);
    }
    for (int k = 20; k <= 53; ++k) {
        at(std::ldexp(1.0, -k));
    }
}

// Q(Phi(x)) is x again, to within the resolution of p = Phi(x): rounding Phi(x) to a double moves its quantile by up
// to ulp(p) / (2 phi(x)), which is large where p nears 1 (0.02 at x = 8), and Halley's step lands a few ulps of x
// from there. Measured, 3.6 times ulp(x) + ulp(p) / phi(x) at most; 8 are allowed.
void normal_quantile_round_trips() {
    Worst worst;
    constexpr int steps = 800000;
    for (int i = 0; i <= steps; ++i) {
        const double x = -8.0 + 16.0 * i / steps;
        const double p = math::normal_cdf(x);
        worst.add(std::fabs(math::normal_quantile(p) - x) / (ulp(x) + ulp(p) / density(x)), x);
    }
    expect_at_most(worst, 8.0, "Q(Phi(x)) - x, in units of ulp(x) + ulp(Phi(x)) / phi(x)");
}

// Non-decreasing down the lower tail to the smallest double, through the switch to Newton's method near Phi(-37),
// across (0.001, 0.999), and up the upper tail to 1 - 1e-16. Single ulps of p are another matter: rounding moves a
// quantile an ulp or two either way (measured near 0.1, and across the switch), so the grids step by far more.
void normal_quantile_is_monotonic() {
    double previous = -inf;
    double previous_p = 0.0;
    const auto at = [&](double p) {
        const double x = math::normal_quantile(p);
        if (x < previous) {
            throw check::Failure("Q(" + text_of(p) + ") = " + text_of(x) + " is below Q(" + text_of(previous_p) +
                                 ") = " + text_of(previous));
        }
        previous = x;
        previous_p = p;
    };
    constexpr int steps = 400000;
    for (int i = 0; i <= steps; ++i) {
        at(std::pow(10.0, -323.3 + 320.3 * i / steps));
    }
    for (int i = 1; i < steps; ++i) {
        at(0.001 + 0.998 * i / steps);
    }
    for (int i = 0; i <= steps; ++i) {
        at(1.0 - std::pow(10.0, -3.0 - 13.0 * i / steps));
    }
}

// Halley's step converges on the root of normal_cdf(x) = p, so in the lower tail the quantile is as good as normal_cdf
// is relative to p: an error e in Phi moves x by e Phi / phi, about e / |x|, which with normal_cdf's 8.9e-9 near
// x = -7.8 is 1.1e-9, 1.4e-10 of x. 2e-10 of x is allowed against values worked to 60 digits, and a dense sweep checks
// that Phi(Q(p)), from erfc, is p to within normal_cdf's 1e-8.
void normal_quantile_lower_tail() {
    const Quantile quantiles[] = {{1e-5, -4.264890793922824610234},
                                  {1e-10, -6.3613409024040561991},
                                  {1e-15, -7.94134532617099677133},
                                  {1e-20, -9.262340089798407579572},
                                  {1e-50, -14.93333753478848898066},
                                  {1e-100, -21.27345356096532429418},
                                  {1e-200, -30.20559417957964306312},
                                  {1e-290, -36.42073167320799882717},
                                  {6e-300, -36.99873557235849017851}};
    for (const Quantile& known : quantiles) {
        check::expect_near(math::normal_quantile(known.p),
                           known.x,
                           2e-10 * std::fabs(known.x),
                           "the quantile of " + text_of(known.p));
    }
    Worst worst;
    constexpr int steps = 200000;
    const double top = std::log10(0.02425);
    for (int i = 0; i <= steps; ++i) {
        const double p = std::pow(10.0, -299.0 + (299.0 + top) * i / steps);
        worst.add(std::fabs(reference_cdf(math::normal_quantile(p)) / p - 1.0), p);
    }
    expect_at_most(worst, 1e-8, "|Phi(Q(p)) / p - 1| down the lower tail", "p");
}

// Below Phi(-37) = 5.7e-300, where normal_cdf has underflowed to 0, the quantile refines Acklam's estimate by Newton's
// method against log Phi, from its asymptotic series, which out there is as good as a double. Against values worked to
// 60 digits, from just below the cutoff down to the smallest double, it is correctly rounded (measured), and 2 ulps
// are allowed. A dense sweep down to 1e-307, while erfc's results are normal doubles, checks Phi(Q(p)) against p:
// an ulp of x moves Phi by 2.7e-13 of itself at x = -37.4, and 3.6e-13 is measured, so 1e-12 is allowed. (Halley's
// step against normal_cdf, used before, moved x 0.018 toward 0 here, and gave NaN below 5.9e-311.)
void normal_quantile_far_tail() {
    const Quantile quantiles[] = {{5.725e-300, -37.00000269457186272591},
                                  {5.7e-300, -37.00012088855887611515},
                                  {1e-300, -37.04709629936119923655},
                                  {1e-305, -37.35634609306710368901},
                                  {smallest_normal, -37.51937934714449982068},
                                  {1e-308, -37.54067492154594240093},
                                  {1e-310, -37.66306033194952373189},
                                  {5.88e-311, -37.67714722974675773298},
                                  {1e-320, -38.26912534303265101818},
                                  {smallest, -38.46740561714434625078}};
    for (const Quantile& known : quantiles) {
        const double x = math::normal_quantile(known.p);
        check::expect(ulps_apart(x, known.x) <= 2.0,
                      "the quantile of " + text_of(known.p) + " is " + text_of(x) + ", not " + text_of(known.x));
    }
    Worst worst;
    constexpr int steps = 100000;
    const double top = std::log10(5.72e-300);
    for (int i = 0; i <= steps; ++i) {
        const double p = std::pow(10.0, -307.0 + (307.0 + top) * i / steps);
        worst.add(std::fabs(reference_cdf(math::normal_quantile(p)) / p - 1.0), p);
    }
    expect_at_most(worst, 1e-12, "|Phi(Q(p)) / p - 1| below Phi(-37)", "p");
}

// ---- The constants ----

// The doubles nearest Euler's constant and e, worked to 60 digits; what exp and log make of e; and both again from
// their series: e as the sum of 1/k!, smallest terms first, and Euler's constant as H(n) - ln n - 1/(2n) + 1/(12n^2) -
// 1/(120n^4), whose next term is 1/(252n^6), summed with Kahan's compensation.
void constants() {
    check::expect_equal(math::euler_gamma, 0x1.2788cfc6fb619p-1, "euler_gamma, 0.57721566490153286");
    check::expect_equal(math::e, 0x1.5bf0a8b145769p+1, "e, 2.7182818284590451");
    check::expect(ulps_apart(math::exp(1.0), math::e) <= 1.0, "exp(1) is within an ulp of e");
    check::expect_equal(math::log(math::e), 1.0, "log(e)");

    double series = 0.0;
    for (int k = 20; k >= 0; --k) {
        double term = 1.0;
        for (int j = 2; j <= k; ++j) {
            term /= j;
        }
        series += term;
    }
    check::expect(ulps_apart(series, math::e) <= 1.0, "e as the sum of 1/k!: " + text_of(series));

    constexpr int n = 1000;
    double harmonic = 0.0;
    double compensation = 0.0;
    for (int k = n; k >= 1; --k) {
        const double term = 1.0 / k - compensation;
        const double sum = harmonic + term;
        compensation = (sum - harmonic) - term;
        harmonic = sum;
    }
    const double m = n;
    const double gamma =
        harmonic - std::log(m) - 1.0 / (2.0 * m) + 1.0 / (12.0 * m * m) - 1.0 / (120.0 * m * m * m * m);
    check::expect_near(gamma, math::euler_gamma, 1e-14, "Euler's constant from the harmonic numbers");
}

}  // namespace

int main() {
    return check::run({
        {"exp against std::exp, within an ulp", exp_against_std},
        {"log against std::log, within an ulp", log_against_std},
        {"pow against std::pow, within 2 + 3 |y ln x| ulps", pow_against_std},
        {"exp of 0, the infinities, NaN, and past overflow and underflow", exp_special_values},
        {"log of 1, 0, negatives, the infinities, and NaN", log_special_values},
        {"pow: IEEE 754's x^0, 1^y, zeros, negative bases, the infinities, NaN, overflow", pow_special_values},
        {"pow of negative bases: whole powers by parity, fractional ones NaN", pow_negative_bases},
        {"pow to infinite powers, and of infinities to finite ones, as IEEE 754 has them", pow_infinite_cases},
        {"normal_cdf against erfc", normal_cdf_against_erfc},
        {"normal_cdf(x) + normal_cdf(-x) is exactly 1", normal_cdf_symmetry},
        {"normal_cdf at known points and at its ends", normal_cdf_known_values},
        {"normal_cdf is non-decreasing", normal_cdf_is_monotonic},
        {"normal_quantile at known points", normal_quantile_known_values},
        {"normal_quantile of 0, 1, and outside [0, 1]", normal_quantile_ends},
        {"normal_quantile(1 - p) is -normal_quantile(p)", normal_quantile_symmetry},
        {"normal_quantile(normal_cdf(x)) is x over [-8, 8]", normal_quantile_round_trips},
        {"normal_quantile is non-decreasing", normal_quantile_is_monotonic},
        {"normal_quantile down the lower tail to 6e-300", normal_quantile_lower_tail},
        {"normal_quantile below 5.7e-300, down to the smallest double", normal_quantile_far_tail},
        {"euler_gamma and e", constants},
    });
}
