#include "nanotrade/math.hpp"

#include <cmath>
#include <limits>

namespace nanotrade::math {
namespace {

constexpr double ln2_hi = 6.93147180369123816490e-01;  // the high 32 bits of ln 2, so k * ln2_hi is exact
constexpr double ln2_lo = 1.90821492927058770002e-10;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();

}  // namespace

double exp(double x) noexcept {
    if (std::isnan(x)) {
        return x;
    }
    if (x > 709.782712893383973096) {
        return infinity;
    }
    if (x < -745.13321910194110842) {
        return 0.0;
    }
    constexpr double inv_ln2 = 1.44269504088896338700e+00;
    constexpr double p1 = 1.66666666666666019037e-01;
    constexpr double p2 = -2.77777777770155933842e-03;
    constexpr double p3 = 6.61375632143793436117e-05;
    constexpr double p4 = -1.65339022054652515390e-06;
    constexpr double p5 = 4.13813679705723846039e-08;

    // x = k ln 2 + r with |r| <= ln 2 / 2, then exp(r) from a rational approximation, then scaled by 2^k.
    const double scaled = inv_ln2 * x;
    const int k = static_cast<int>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    const double hi = x - k * ln2_hi;
    const double lo = k * ln2_lo;
    const double r = hi - lo;
    const double r2 = r * r;
    const double c = r - r2 * (p1 + r2 * (p2 + r2 * (p3 + r2 * (p4 + r2 * p5))));
    const double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
    return std::ldexp(y, k);
}

double log(double x) noexcept {
    if (std::isnan(x) || x < 0.0) {
        return nan;
    }
    if (x == 0.0) {
        return -infinity;
    }
    if (std::isinf(x)) {
        return x;
    }
    constexpr double lg1 = 6.666666666666735130e-01;
    constexpr double lg2 = 3.999999999940941908e-01;
    constexpr double lg3 = 2.857142874366239149e-01;
    constexpr double lg4 = 2.222219843214978396e-01;
    constexpr double lg5 = 1.818357216161805012e-01;
    constexpr double lg6 = 1.531383769920937332e-01;
    constexpr double lg7 = 1.479819860511658591e-01;

    // x = 2^k (1 + f) with sqrt(2)/2 <= 1 + f < sqrt(2); log(1 + f) = 2s + 2s^3/3 + ... with s = f / (2 + f).
    int k = 0;
    double m = std::frexp(x, &k);
    if (m < 0.70710678118654752440) {
        m *= 2.0;
        --k;
    }
    const double f = m - 1.0;
    const double dk = k;
    const double s = f / (2.0 + f);
    const double z = s * s;
    const double w = z * z;
    const double t1 = w * (lg2 + w * (lg4 + w * lg6));
    const double t2 = z * (lg1 + w * (lg3 + w * (lg5 + w * lg7)));
    const double r = t2 + t1;
    const double hfsq = 0.5 * f * f;
    return dk * ln2_hi - ((hfsq - (s * (hfsq + r) + dk * ln2_lo)) - f);
}

double pow(double base, double exponent) noexcept {
    if (exponent == 0.0 || base == 1.0) {
        return 1.0;
    }
    if (std::isnan(base) || std::isnan(exponent) || base < 0.0) {
        return nan;
    }
    if (base == 0.0) {
        return exponent > 0.0 ? 0.0 : infinity;
    }
    return exp(exponent * log(base));
}

double normal_cdf(double x) noexcept {
    if (std::isnan(x)) {
        return x;
    }
    // Hart's algorithm 5666, as given by West, "Better approximations to cumulative normal functions" (2005).
    const double z = std::fabs(x);
    double tail = 0.0;
    if (z <= 37.0) {
        const double density = exp(-z * z / 2.0);
        if (z < 7.07106781186547) {
            double numerator = 3.52624965998911e-02 * z + 0.700383064443688;
            numerator = numerator * z + 6.37396220353165;
            numerator = numerator * z + 33.912866078383;
            numerator = numerator * z + 112.079291497871;
            numerator = numerator * z + 221.213596169931;
            numerator = numerator * z + 220.206867912376;
            double denominator = 8.83883476483184e-02 * z + 1.75566716318264;
            denominator = denominator * z + 16.064177579207;
            denominator = denominator * z + 86.7807322029461;
            denominator = denominator * z + 296.564248779674;
            denominator = denominator * z + 637.333633378831;
            denominator = denominator * z + 793.826512519948;
            denominator = denominator * z + 440.413735824752;
            tail = density * numerator / denominator;
        } else {
            double fraction = z + 0.65;
            fraction = z + 4.0 / fraction;
            fraction = z + 3.0 / fraction;
            fraction = z + 2.0 / fraction;
            fraction = z + 1.0 / fraction;
            tail = density / fraction / 2.506628274631;
        }
    }
    return x > 0.0 ? 1.0 - tail : tail;
}

double normal_quantile(double p) noexcept {
    if (!(p > 0.0 && p < 1.0)) {
        if (p == 0.0) {
            return -infinity;
        }
        if (p == 1.0) {
            return infinity;
        }
        return nan;
    }
    // Acklam's rational approximation, then one step of Halley's method against normal_cdf.
    constexpr double a[] = {-3.969683028665376e+01,
                            2.209460984245205e+02,
                            -2.759285104469687e+02,
                            1.383577518672690e+02,
                            -3.066479806614716e+01,
                            2.506628277459239e+00};
    constexpr double b[] = {-5.447609879822406e+01,
                            1.615858368580409e+02,
                            -1.556989798598866e+02,
                            6.680131188771972e+01,
                            -1.328068155288572e+01};
    constexpr double c[] = {-7.784894002430293e-03,
                            -3.223964580411365e-01,
                            -2.400758277161838e+00,
                            -2.549732539343734e+00,
                            4.374664141464968e+00,
                            2.938163982698783e+00};
    constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    constexpr double low = 0.02425;

    double x = 0.0;
    if (p < low || p > 1.0 - low) {
        const double q = std::sqrt(-2.0 * log(p < low ? p : 1.0 - p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
        if (p > 1.0 - low) {
            x = -x;
        }
    } else {
        const double q = p - 0.5;
        const double r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
    }
    const double error = normal_cdf(x) - p;
    const double u = error * 2.50662827463100050242 * exp(x * x / 2.0);
    return x - u / (1.0 + x * u / 2.0);
}

}  // namespace nanotrade::math
