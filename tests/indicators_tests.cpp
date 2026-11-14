// The indicators against slow references written from their definitions, on random walks with gaps in them, plus
// small cases worked by hand, the warm-up of each, and the shared cache.

#include "check.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/random.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ind = nanotrade::indicators;
using ind::Series;
using nanotrade::BarSeries;
using nanotrade::Random;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double relative_tolerance = 1e-9;

const std::vector<std::size_t> periods = {1, 2, 3, 4, 5, 7, 10, 14, 20, 26, 50, 100, 200, 500};

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// ---- References, written from the definitions with no attempt at speed ----

bool full_window(const Series& x, std::size_t i, std::size_t n) {
    if (i + 1 < n) {
        return false;
    }
    for (std::size_t j = i + 1 - n; j <= i; ++j) {
        if (std::isnan(x[j])) {
            return false;
        }
    }
    return true;
}

double window_mean(const Series& x, std::size_t i, std::size_t n) {
    double sum = 0.0;
    for (std::size_t j = i + 1 - n; j <= i; ++j) {
        sum += x[j];
    }
    return sum / static_cast<double>(n);
}

Series ref_sma(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (full_window(x, i, n)) {
            out[i] = window_mean(x, i, n);
        }
    }
    return out;
}

// Population standard deviation, in two passes over each window.
Series ref_stdev(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!full_window(x, i, n)) {
            continue;
        }
        const double mean = window_mean(x, i, n);
        double squares = 0.0;
        for (std::size_t j = i + 1 - n; j <= i; ++j) {
            squares += (x[j] - mean) * (x[j] - mean);
        }
        out[i] = std::sqrt(squares / static_cast<double>(n));
    }
    return out;
}

Series ref_extreme(const Series& x, std::size_t n, bool highest) {
    Series out(x.size(), nan);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!full_window(x, i, n)) {
            continue;
        }
        double best = x[i];
        for (std::size_t j = i + 1 - n; j <= i; ++j) {
            best = highest ? std::max(best, x[j]) : std::min(best, x[j]);
        }
        out[i] = best;
    }
    return out;
}

// Exponential smoothing that starts over after every missing value: the first n values of each run are averaged into
// the seed, and every later one is blended in as alpha * x + (1 - alpha) * previous.
Series ref_smooth(const Series& x, std::size_t n, double alpha) {
    Series out(x.size(), nan);
    std::size_t run_start = 0;
    double value = nan;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (std::isnan(x[i])) {
            run_start = i + 1;
            continue;
        }
        const std::size_t length = i - run_start + 1;
        if (length < n) {
            continue;
        }
        value = length == n ? window_mean(x, i, n) : alpha * x[i] + (1.0 - alpha) * value;
        out[i] = value;
    }
    return out;
}

Series ref_ema(const Series& x, std::size_t n) {
    return ref_smooth(x, n, 2.0 / (static_cast<double>(n) + 1.0));
}

// Wilder's RSI: the first n changes of a run of defined values are averaged, and each later change is blended in with
// weight 1/n; the RSI is 100 g / (g + l).
Series ref_rsi(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    const auto period = static_cast<double>(n);
    std::size_t changes = 0;
    double gain = 0.0;
    double loss = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (std::isnan(x[i]) || std::isnan(x[i - 1])) {
            changes = 0;
            continue;
        }
        ++changes;
        if (changes < n) {
            continue;
        }
        if (changes == n) {
            gain = 0.0;
            loss = 0.0;
            for (std::size_t j = i + 1 - n; j <= i; ++j) {
                const double d = x[j] - x[j - 1];
                gain += d > 0.0 ? d : 0.0;
                loss += d < 0.0 ? -d : 0.0;
            }
            gain /= period;
            loss /= period;
        } else {
            const double d = x[i] - x[i - 1];
            gain += ((d > 0.0 ? d : 0.0) - gain) / period;
            loss += ((d < 0.0 ? -d : 0.0) - loss) / period;
        }
        if (gain == 0.0 && loss == 0.0) {
            out[i] = 50.0;
        } else if (loss == 0.0) {
            out[i] = 100.0;
        } else {
            out[i] = 100.0 * gain / (gain + loss);
        }
    }
    return out;
}

Series ref_change(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    for (std::size_t i = n; i < x.size(); ++i) {
        out[i] = x[i] - x[i - n];
    }
    return out;
}

Series ref_roc(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    for (std::size_t i = n; i < x.size(); ++i) {
        if (!std::isnan(x[i]) && !std::isnan(x[i - n]) && x[i - n] != 0.0) {
            out[i] = (x[i] - x[i - n]) / x[i - n];
        }
    }
    return out;
}

Series ref_zscore(const Series& x, std::size_t n) {
    const Series mean = ref_sma(x, n);
    const Series deviation = ref_stdev(x, n);
    Series out(x.size(), nan);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isnan(deviation[i]) && deviation[i] > 0.0) {
            out[i] = (x[i] - mean[i]) / deviation[i];
        }
    }
    return out;
}

Series ref_lag(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    for (std::size_t i = 0; i + n < x.size(); ++i) {
        out[i + n] = x[i];
    }
    return out;
}

// The true range as the span from the lower of the low and the previous close to the higher of the high and it.
Series ref_true_range(const BarSeries& bars) {
    Series out(bars.size(), nan);
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const double high = bars.high[i];
        const double low = bars.low[i];
        const double previous = bars.close[i - 1];
        if (std::isnan(high) || std::isnan(low) || std::isnan(previous)) {
            continue;
        }
        out[i] = std::max(high, previous) - std::min(low, previous);
    }
    return out;
}

Series ref_atr(const BarSeries& bars, std::size_t n) {
    return ref_smooth(ref_true_range(bars), n, 1.0 / static_cast<double>(n));
}

// ---- Inputs ----

Series random_walk(std::uint64_t seed, std::size_t count) {
    Random random(seed);
    Series x(count);
    double value = 100.0;
    for (double& v : x) {
        value *= std::exp(0.02 * random.normal());
        v = value;
    }
    return x;
}

// Single missing values here and there, a few longer gaps, and one at the very start.
Series with_gaps(Series x, std::uint64_t seed) {
    Random random(seed);
    for (double& v : x) {
        if (random.uniform() < 0.003) {
            v = nan;
        }
    }
    for (int gap = 0; gap < 4; ++gap) {
        const std::size_t start = random.below(x.size());
        const std::size_t length = 1 + random.below(40);
        for (std::size_t i = start; i < std::min(x.size(), start + length); ++i) {
            x[i] = nan;
        }
    }
    x[0] = nan;
    return x;
}

BarSeries bars_from(const Series& close, std::uint64_t seed) {
    Random random(seed);
    BarSeries bars;
    bars.symbol = "WALK";
    const std::int64_t first = nanotrade::days_from_civil(2001, 1, 1);
    double previous = close.front();
    for (std::size_t i = 0; i < close.size(); ++i) {
        const double open = previous * std::exp(0.005 * random.normal());
        const double high = std::max(open, close[i]) * std::exp(0.01 * std::fabs(random.normal()));
        const double low = std::min(open, close[i]) * std::exp(-0.01 * std::fabs(random.normal()));
        bars.push_back((first + static_cast<std::int64_t>(i)) * nanotrade::seconds_per_day,
                       open,
                       high,
                       low,
                       close[i],
                       1000.0);
        previous = close[i];
    }
    return bars;
}

void expect_series(const Series& actual,
                   const Series& expected,
                   const std::string& what,
                   double tolerance = relative_tolerance) {
    check::expect_equal(actual.size(), expected.size(), what + ": length");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const bool actual_missing = std::isnan(actual[i]);
        const bool expected_missing = std::isnan(expected[i]);
        if (actual_missing || expected_missing) {
            check::expect(actual_missing && expected_missing,
                          what + " at " + std::to_string(i) + ": expected " + text_of(expected[i]) + ", got " +
                              text_of(actual[i]));
            continue;
        }
        check::expect_near(actual[i],
                           expected[i],
                           tolerance * std::max(1.0, std::fabs(expected[i])),
                           what + " at " + std::to_string(i));
    }
}

std::size_t defined_count(const Series& x) {
    return static_cast<std::size_t>(std::count_if(x.begin(), x.end(), [](double v) { return !std::isnan(v); }));
}

std::size_t first_defined(const Series& x) {
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isnan(x[i])) {
            return i;
        }
    }
    return x.size();
}

using Indicator = std::function<Series(const Series&, std::size_t)>;

struct Pair {
    const char* name;
    Indicator engine;
    Indicator reference;
};

const std::vector<Pair>& pairs() {
    static const std::vector<Pair> list = {
        {"sma", [](const Series& x, std::size_t n) { return ind::sma(x, n); }, ref_sma},
        {"ema", [](const Series& x, std::size_t n) { return ind::ema(x, n); }, ref_ema},
        {"rsi", [](const Series& x, std::size_t n) { return ind::rsi(x, n); }, ref_rsi},
        {"stdev", [](const Series& x, std::size_t n) { return ind::stdev(x, n); }, ref_stdev},
        {"highest",
         [](const Series& x, std::size_t n) { return ind::highest(x, n); },
         [](const Series& x, std::size_t n) { return ref_extreme(x, n, true); }},
        {"lowest",
         [](const Series& x, std::size_t n) { return ind::lowest(x, n); },
         [](const Series& x, std::size_t n) { return ref_extreme(x, n, false); }},
        {"change", [](const Series& x, std::size_t n) { return ind::change(x, n); }, ref_change},
        {"roc", [](const Series& x, std::size_t n) { return ind::roc(x, n); }, ref_roc},
        {"lag", [](const Series& x, std::size_t n) { return ind::lag(x, n); }, ref_lag},
    };
    return list;
}

void run_differential(const Series& x, const std::string& label) {
    for (const Pair& pair : pairs()) {
        for (const std::size_t n : periods) {
            const std::string what = label + " " + pair.name + "(" + std::to_string(n) + ")";
            const Series expected = pair.reference(x, n);
            expect_series(pair.engine(x, n), expected, what);
        }
    }
}

// ---- Differential tests ----

void differential_clean() {
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        const Series x = random_walk(seed, 3000);
        run_differential(x, "walk " + std::to_string(seed));
        // Every indicator has values for every period on a walk without gaps.
        for (const Pair& pair : pairs()) {
            check::expect(defined_count(pair.engine(x, 500)) > 2000, std::string(pair.name) + " has values");
        }
    }
}

void differential_gaps() {
    std::size_t defined = 0;
    for (std::uint64_t seed = 11; seed <= 13; ++seed) {
        const Series x = with_gaps(random_walk(seed, 4000), seed * 7);
        run_differential(x, "gappy walk " + std::to_string(seed));
        defined += defined_count(ind::sma(x, 200));
    }
    check::expect(defined > 1000, "the gaps leave long stretches to compute over");
}

// Periods of 1 are left out: the z-score of a single value is 0 / 0 (see zscore_flat_window).
void differential_zscore() {
    for (std::uint64_t seed = 21; seed <= 23; ++seed) {
        const Series clean = random_walk(seed, 3000);
        const Series gappy = with_gaps(clean, seed);
        for (const std::size_t n : periods) {
            if (n == 1) {
                continue;
            }
            expect_series(ind::zscore(clean, n), ref_zscore(clean, n), "zscore(" + std::to_string(n) + ")");
            expect_series(ind::zscore(gappy, n), ref_zscore(gappy, n), "gappy zscore(" + std::to_string(n) + ")");
        }
    }
}

// Missing highs and lows; a missing close is covered by true_range_missing_previous_close.
void differential_atr() {
    for (std::uint64_t seed = 31; seed <= 33; ++seed) {
        const Series close = random_walk(seed, 3000);
        BarSeries bars = bars_from(close, seed);
        expect_series(ind::true_range(bars), ref_true_range(bars), "true_range");
        for (const std::size_t n : periods) {
            expect_series(ind::atr(bars, n), ref_atr(bars, n), "atr(" + std::to_string(n) + ")");
        }
        Random random(seed);
        for (std::size_t i = 0; i < bars.size(); ++i) {
            if (random.uniform() < 0.004) {
                (random.uniform() < 0.5 ? bars.high[i] : bars.low[i]) = nan;
            }
        }
        expect_series(ind::true_range(bars), ref_true_range(bars), "gappy true_range");
        for (const std::size_t n : periods) {
            expect_series(ind::atr(bars, n), ref_atr(bars, n), "gappy atr(" + std::to_string(n) + ")");
        }
    }
}

// An indicator of an indicator, as the strategy language builds them: the EMA of an RSI starts once the RSI has.
void differential_composed() {
    const Series x = with_gaps(random_walk(41, 3000), 41);
    for (const std::size_t n : {2, 5, 14}) {
        const Series rsi = ind::rsi(x, n);
        for (const std::size_t m : {1, 3, 9, 30}) {
            expect_series(ind::ema(rsi, m), ref_ema(ref_rsi(x, n), m), "ema of rsi");
            expect_series(ind::sma(rsi, m), ref_sma(ref_rsi(x, n), m), "sma of rsi");
            expect_series(ind::highest(rsi, m), ref_extreme(ref_rsi(x, n), m, true), "highest of rsi");
        }
    }
}

// ---- Small cases worked by hand ----

void hand_sma() {
    expect_series(ind::sma(Series{1, 2, 3, 4, 5}, 3), {nan, nan, 2, 3, 4}, "sma 3", 1e-15);
    expect_series(ind::sma(Series{1, 2, 3, 4, 5}, 1), {1, 2, 3, 4, 5}, "sma 1", 1e-15);
    expect_series(ind::sma(Series{1, 2, 3, 4, 5}, 5), {nan, nan, nan, nan, 3}, "sma 5", 1e-15);
    expect_series(ind::sma(Series{1, 2, 3, 4, 5}, 6), {nan, nan, nan, nan, nan}, "sma longer than the data", 1e-15);
    expect_series(ind::sma(Series{1, 2, nan, 4, 5, 6, 7}, 2),
                  {nan, 1.5, nan, nan, 4.5, 5.5, 6.5},
                  "sma over a gap",
                  1e-15);
    check::expect(ind::sma(Series{}, 3).empty(), "sma of nothing");
}

void hand_ema() {
    // alpha = 2 / (3 + 1) = 0.5, seeded with the mean of 2, 4, 6.
    expect_series(ind::ema(Series{2, 4, 6, 8, 20}, 3), {nan, nan, 4, 6, 13}, "ema 3", 1e-15);
    // alpha = 2/3; the gap restarts the seed: (8 + 10) / 2, then 9 + 2/3 (12 - 9), then 11 + 2/3 (14 - 11).
    expect_series(ind::ema(Series{2, 4, 6, nan, 8, 10, 12, 14}, 2), {nan, 3, 5, nan, nan, 9, 11, 13}, "ema 2", 1e-12);
    expect_series(ind::ema(Series{5, 7, 9}, 1), {5, 7, 9}, "ema 1 follows the input", 1e-15);
}

void hand_rsi() {
    // Changes +1, -1, +1, +1. Seed: gain 0.5, loss 0.5: 50. Then gain (0.5 + 1) / 2 = 0.75 and loss 0.25: RS 3,
    // RSI 75. Then 0.875 and 0.125: RS 7, RSI 87.5.
    expect_series(ind::rsi(Series{1, 2, 1, 2, 3}, 2), {nan, nan, 50, 75, 87.5}, "rsi 2", 1e-12);
    expect_series(ind::rsi(Series{5, 5, 5, 5}, 2), {nan, nan, 50, 50}, "flat: neither gains nor losses", 0.0);
    expect_series(ind::rsi(Series{1, 2, 3, 4}, 2), {nan, nan, 100, 100}, "only gains", 0.0);
    expect_series(ind::rsi(Series{4, 3, 2, 1}, 2), {nan, nan, 0, 0}, "only losses", 0.0);
    // A gap restarts the averages: two changes after it are needed again. Then gain (0 + 1) / 2 and loss (1 + 0) / 2.
    expect_series(ind::rsi(Series{1, 2, 3, nan, 3, 2, 1, 2}, 2),
                  {nan, nan, 100, nan, nan, nan, 0, 50},
                  "rsi gap",
                  1e-12);
    check::expect(ind::rsi(Series{1}, 1).size() == 1 && std::isnan(ind::rsi(Series{1}, 1)[0]), "rsi of one value");
}

void hand_stdev() {
    // The classic example: mean 5, population variance 4.
    const Series x{2, 4, 4, 4, 5, 5, 7, 9};
    const Series s = ind::stdev(x, 8);
    check::expect(std::isnan(s[6]), "stdev warm-up");
    check::expect_near(s[7], 2.0, 1e-15, "population stdev");
    expect_series(ind::stdev(Series{1, 3, 3}, 2), {nan, 1, 0}, "stdev 2", 1e-15);
}

void hand_extremes() {
    const Series x{3, 1, 4, 1, 5, 9, 2, 6};
    expect_series(ind::highest(x, 3), {nan, nan, 4, 4, 5, 9, 9, 9}, "highest 3", 0.0);
    expect_series(ind::lowest(x, 3), {nan, nan, 1, 1, 1, 1, 2, 2}, "lowest 3", 0.0);
    expect_series(ind::highest(x, 1), x, "highest 1 is the value itself", 0.0);
    expect_series(ind::lowest(Series{5, nan, 4, 3, 7}, 2), {nan, nan, nan, 3, 3}, "lowest over a gap", 0.0);
    // The current bar counts: a new high is its own highest.
    expect_series(ind::highest(Series{1, 2, 3, 4}, 2), {nan, 2, 3, 4}, "the current bar is included", 0.0);
}

void hand_change_roc_lag() {
    expect_series(ind::change(Series{1, 4, 9, 16, 25}, 2), {nan, nan, 8, 12, 16}, "change 2", 0.0);
    expect_series(ind::roc(Series{100, 110, 121, 0, 50}, 1),
                  {nan, 0.1, 0.1, -1, nan},
                  "roc 1, NaN over a zero base",
                  1e-15);
    expect_series(ind::lag(Series{1, 2, 3, 4}, 1), {nan, 1, 2, 3}, "lag 1", 0.0);
    expect_series(ind::lag(Series{1, 2, 3, 4}, 0), {1, 2, 3, 4}, "lag 0 is the identity", 0.0);
    expect_series(ind::lag(Series{1, 2, 3, 4}, 4), {nan, nan, nan, nan}, "lag beyond the data", 0.0);
    expect_series(ind::lag(Series{1, 2, 3, 4}, 99), {nan, nan, nan, nan}, "lag far beyond the data", 0.0);
}

void hand_zscore() {
    // Mean 2, population deviation sqrt(2/3): z = 1 / sqrt(2/3).
    const Series z = ind::zscore(Series{1, 2, 3}, 3);
    check::expect_near(z[2], std::sqrt(1.5), 1e-15, "zscore");
    check::expect(std::isnan(z[0]) && std::isnan(z[1]), "zscore warm-up");
    check::expect(std::isnan(ind::zscore(Series{4, 4, 4}, 3)[2]), "a flat window has no z-score");
}

BarSeries hand_bars() {
    BarSeries bars;
    const std::array<std::array<double, 4>, 5> rows = {{
        {10, 11, 9, 10},
        {10, 12, 10, 11},   // TR max(2, |12 - 10|, |10 - 10|) = 2
        {11, 11.5, 8, 9},   // TR max(3.5, 0.5, |8 - 11|) = 3.5
        {12, 14, 12, 13},   // TR max(2, |14 - 9|, |12 - 9|) = 5: a gap up
        {13, 13, 12, 12.5}  // TR max(1, |13 - 13|, |12 - 13|) = 1
    }};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        bars.push_back(static_cast<std::int64_t>(i) * nanotrade::seconds_per_day,
                       rows[i][0],
                       rows[i][1],
                       rows[i][2],
                       rows[i][3],
                       0.0);
    }
    return bars;
}

void hand_atr() {
    const BarSeries bars = hand_bars();
    expect_series(ind::true_range(bars), {nan, 2, 3.5, 5, 1}, "true range", 0.0);
    // Seeded with (2 + 3.5) / 2, then 2.75 + (5 - 2.75) / 2, then 3.875 + (1 - 3.875) / 2.
    expect_series(ind::atr(bars, 2), {nan, nan, 2.75, 3.875, 2.4375}, "atr 2", 1e-15);
    expect_series(ind::atr(bars, 4), {nan, nan, nan, nan, 2.875}, "atr 4", 1e-15);
    check::expect(ind::true_range(BarSeries{}).empty(), "true range of no bars");
}

// ---- Warm-up and gaps ----

void warm_up_lengths() {
    const Series x = random_walk(51, 300);
    const BarSeries bars = bars_from(x, 51);
    for (std::size_t n = 1; n <= 40; ++n) {
        const std::string suffix = "(" + std::to_string(n) + ")";
        const auto expect_start = [&](const Series& s, std::size_t start, const std::string& name) {
            check::expect_equal(first_defined(s), start, name + suffix + " starts");
            check::expect_equal(defined_count(s), s.size() - start, name + suffix + " is defined from then on");
        };
        expect_start(ind::sma(x, n), n - 1, "sma");
        expect_start(ind::ema(x, n), n - 1, "ema");
        expect_start(ind::stdev(x, n), n - 1, "stdev");
        expect_start(ind::highest(x, n), n - 1, "highest");
        expect_start(ind::lowest(x, n), n - 1, "lowest");
        expect_start(ind::rsi(x, n), n, "rsi");
        expect_start(ind::change(x, n), n, "change");
        expect_start(ind::roc(x, n), n, "roc");
        expect_start(ind::lag(x, n), n, "lag");
        expect_start(ind::atr(bars, n), n, "atr");
        if (n > 1) {
            expect_start(ind::zscore(x, n), n - 1, "zscore");
        }
    }
}

// One missing value blanks every window that holds it, and the recursive indicators start over after it.
void a_gap_blanks_its_windows() {
    Series x = random_walk(61, 200);
    const std::size_t k = 100;
    x[k] = nan;
    for (const std::size_t n : {1, 2, 5, 20}) {
        const std::string suffix = "(" + std::to_string(n) + ")";
        const auto blank_from_to = [&](const Series& s, std::size_t from, std::size_t to, const std::string& name) {
            for (std::size_t i = 60; i < 160; ++i) {
                const bool blank = i >= from && i <= to;
                check::expect(std::isnan(s[i]) == blank,
                              name + suffix + " at " + std::to_string(i) + (blank ? " is NaN" : " is defined"));
            }
        };
        blank_from_to(ind::sma(x, n), k, k + n - 1, "sma");
        blank_from_to(ind::ema(x, n), k, k + n - 1, "ema");
        blank_from_to(ind::stdev(x, n), k, k + n - 1, "stdev");
        blank_from_to(ind::highest(x, n), k, k + n - 1, "highest");
        blank_from_to(ind::lowest(x, n), k, k + n - 1, "lowest");
        blank_from_to(ind::rsi(x, n), k, k + n, "rsi");
        const Series change = ind::change(x, n);
        for (std::size_t i = 60; i < 160; ++i) {
            check::expect(std::isnan(change[i]) == (i == k || i == k + n),
                          "change" + suffix + " at " + std::to_string(i));
        }
    }
}

void zero_period_throws() {
    const Series x = random_walk(71, 20);
    const BarSeries bars = bars_from(x, 71);
    using nanotrade::Error;
    check::expect_throws<Error>([&] { ind::sma(x, 0); }, "sma");
    check::expect_throws<Error>([&] { ind::ema(x, 0); }, "ema");
    check::expect_throws<Error>([&] { ind::rsi(x, 0); }, "rsi");
    check::expect_throws<Error>([&] { ind::stdev(x, 0); }, "stdev");
    check::expect_throws<Error>([&] { ind::highest(x, 0); }, "highest");
    check::expect_throws<Error>([&] { ind::lowest(x, 0); }, "lowest");
    check::expect_throws<Error>([&] { ind::change(x, 0); }, "change");
    check::expect_throws<Error>([&] { ind::roc(x, 0); }, "roc");
    check::expect_throws<Error>([&] { ind::zscore(x, 0); }, "zscore");
    check::expect_throws<Error>([&] { ind::atr(bars, 0); }, "atr");
    expect_series(ind::lag(x, 0), x, "lag 0 is allowed", 0.0);
}

// ---- Bugs found by these tests ----

// The source promises that "a flat window gives exactly 0", but the mean comes from the SMA's running sum, which
// carries rounding from the values that left the window: after 0.1, 0.7, 0.3, 1.1 the window 5, 5, 5 has a mean of
// 4.9999999999999991, a deviation of 8.9e-16, and a z-score of exactly 1 instead of none.
void zscore_flat_window() {
    const Series x{0.1, 0.7, 0.3, 1.1, 5, 5, 5, 5, 5};
    const Series deviation = ind::stdev(x, 3);
    const Series z = ind::zscore(x, 3);
    for (std::size_t i = 6; i < x.size(); ++i) {
        check::expect_equal(deviation[i], 0.0, "stdev of the flat window ending at " + std::to_string(i));
        check::expect(std::isnan(z[i]),
                      "zscore of the flat window ending at " + std::to_string(i) + " is " + text_of(z[i]));
    }
    // A window of one value never deviates.
    const Series walk = random_walk(81, 500);
    check::expect_equal(defined_count(ind::zscore(walk, 1)), std::size_t{0}, "zscore over one bar is never defined");
}

// true_range takes std::max of {high - low, |high - previous|, |low - previous|}, and std::max skips a NaN unless it
// comes first, so a missing previous close leaves the bar's range defined, unlike a missing high or low.
void true_range_missing_previous_close() {
    BarSeries bars = hand_bars();
    bars.close[1] = nan;
    const Series range = ind::true_range(bars);
    check::expect(std::isnan(range[2]), "the true range after a missing close is unknown, got " + text_of(range[2]));
    check::expect(std::isnan(ind::atr(bars, 2)[3]), "the ATR window holding it is undefined");
}

// ---- The cache ----

BarSeries cache_bars() {
    return bars_from(random_walk(91, 50), 91);
}

void cache_computes_once() {
    const BarSeries bars = cache_bars();
    ind::Cache cache(bars);
    check::expect(&cache.bars() == &bars, "the cache refers to its bars");
    int calls = 0;
    const auto make = [&] {
        ++calls;
        return Series{1, 2, 3};
    };
    const Series& first = cache.get("k", make);
    const Series& second = cache.get("k", make);
    const Series& third = cache.get("k", [&]() -> Series { throw std::runtime_error("must not be called"); });
    check::expect_equal(calls, 1, "computed once");
    check::expect(&first == &second && &second == &third, "the same series every time");
    check::expect(first == Series({1, 2, 3}), "its values");
    check::expect_equal(cache.size(), std::size_t{1}, "one entry");
    cache.get("other", make);
    check::expect_equal(calls, 2, "another key is computed");
    check::expect_equal(cache.size(), std::size_t{2}, "two entries");
    check::expect(&cache.get("k", make) == &first, "earlier entries stay put as the cache grows");
}

void cache_one_computation_across_threads() {
    const BarSeries bars = cache_bars();
    for (int round = 0; round < 5; ++round) {
        ind::Cache cache(bars);
        constexpr int threads = 16;
        std::atomic<int> calls{0};
        std::atomic<int> ready{0};
        std::vector<const Series*> seen(threads, nullptr);
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                ready.fetch_add(1);
                while (ready.load() < threads) {
                    std::this_thread::yield();
                }
                seen[static_cast<std::size_t>(t)] = &cache.get("slow", [&] {
                    calls.fetch_add(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    return Series{4, 5, 6};
                });
            });
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
        check::expect_equal(calls.load(), 1, "16 threads asking at once compute the key once");
        for (const Series* s : seen) {
            check::expect(s == seen.front(), "every thread gets the same series");
        }
        check::expect(*seen.front() == Series({4, 5, 6}), "the computed values");
    }
}

void cache_many_keys_across_threads() {
    const BarSeries bars = cache_bars();
    ind::Cache cache(bars);
    constexpr std::size_t keys = 64;
    constexpr std::size_t threads = 16;
    std::array<std::atomic<int>, keys> calls{};
    std::vector<std::thread> pool;
    std::atomic<bool> wrong{false};
    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (std::size_t j = 0; j < keys; ++j) {
                const std::size_t key = (j * 7 + t * 13) % keys;
                const Series& values = cache.get("key" + std::to_string(key), [&] {
                    calls[key].fetch_add(1);
                    // Asks the cache for a column while computing, as an indicator over a price does.
                    const Series& close = cache.column("close");
                    return Series{static_cast<double>(key), close[0]};
                });
                if (values.size() != 2 || values[0] != static_cast<double>(key) || values[1] != bars.close[0]) {
                    wrong.store(true);
                }
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    check::expect(!wrong.load(), "every thread saw the right values");
    for (std::size_t key = 0; key < keys; ++key) {
        check::expect_equal(calls[key].load(), 1, "key " + std::to_string(key) + " computed once");
    }
    check::expect_equal(cache.size(), keys + 1, "the keys and the close");
}

void cache_nested_and_failed_computations() {
    const BarSeries bars = cache_bars();
    ind::Cache cache(bars);
    const Series& outer = cache.get("outer", [&] {
        const Series& inner = cache.get("inner", [] { return Series{1.0}; });
        return Series{inner[0] + 1.0};
    });
    check::expect(outer == Series({2.0}), "a computation can ask the cache for another key");
    check::expect_equal(cache.size(), std::size_t{2}, "both keys are kept");

    int attempts = 0;
    check::expect_throws<std::runtime_error>(
        [&] {
            cache.get("flaky", [&]() -> Series {
                ++attempts;
                throw std::runtime_error("failed");
            });
        },
        "a failed computation throws to the caller");
    const Series& retried = cache.get("flaky", [&] {
        ++attempts;
        return Series{3.0};
    });
    check::expect(retried == Series({3.0}), "a failed key is computed again next time");
    check::expect_equal(attempts, 2, "two attempts");
}

void cache_columns() {
    const BarSeries bars = cache_bars();
    ind::Cache cache(bars);
    check::expect(cache.column("open") == bars.open, "open");
    check::expect(cache.column("high") == bars.high, "high");
    check::expect(cache.column("low") == bars.low, "low");
    check::expect(cache.column("close") == bars.close, "close");
    check::expect(cache.column("volume") == bars.volume, "volume");
    const Series& hl2 = cache.column("hl2");
    check::expect_equal(hl2.size(), bars.size(), "hl2 length");
    for (std::size_t i = 0; i < bars.size(); ++i) {
        check::expect_equal(hl2[i], (bars.high[i] + bars.low[i]) / 2.0, "hl2 is the midpoint");
    }
    check::expect(&cache.column("close") == &cache.column("close"), "a column is kept");
    check::expect(&cache.column("close") == &cache.get("close", [] { return Series{}; }), "under its own name");
    check::expect_throws<nanotrade::Error>([&] { cache.column("vwap"); }, "an unknown column");
    check::expect_throws<nanotrade::Error>([&] { cache.column("Close"); }, "names are lowercase");
}

}  // namespace

int main() {
    return check::run({
        {"sma, ema, rsi, stdev, highest, lowest, change, roc, lag match references", differential_clean},
        {"the same over gaps", differential_gaps},
        {"zscore matches its reference", differential_zscore},
        {"true range and atr match references", differential_atr},
        {"indicators of indicators match references", differential_composed},
        {"sma by hand", hand_sma},
        {"ema by hand", hand_ema},
        {"rsi by hand", hand_rsi},
        {"stdev by hand", hand_stdev},
        {"highest and lowest by hand", hand_extremes},
        {"change, roc, and lag by hand", hand_change_roc_lag},
        {"zscore by hand", hand_zscore},
        {"true range and atr by hand", hand_atr},
        {"warm-up lengths", warm_up_lengths},
        {"a missing value blanks every window holding it", a_gap_blanks_its_windows},
        {"a period of 0 throws", zero_period_throws},
        {"a flat window has no deviation and no z-score", zscore_flat_window},
        {"the true range after a missing close is undefined", true_range_missing_previous_close},
        {"the cache computes a key once", cache_computes_once},
        {"16 threads asking for one key compute it once", cache_one_computation_across_threads},
        {"many keys from many threads", cache_many_keys_across_threads},
        {"nested and failed computations", cache_nested_and_failed_computations},
        {"price columns", cache_columns},
    });
}
