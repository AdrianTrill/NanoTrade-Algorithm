// The performance metrics against values worked out here from their definitions: equity curves built by hand over
// bars with known dates, data whose moments are known exactly, and random curves, on which the drawdowns, the metrics,
// and the calendar must agree with one another.

#include "check.hpp"

#include "nanotrade/engine.hpp"
#include "nanotrade/metrics.hpp"
#include "nanotrade/random.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using nanotrade::BacktestResult;
using nanotrade::BarSeries;
using nanotrade::CivilDate;
using nanotrade::DrawdownPeriod;
using nanotrade::DrawdownSpan;
using nanotrade::Metrics;
using nanotrade::ReturnMoments;
using nanotrade::Timestamp;
using nanotrade::Trade;
using nanotrade::YearReturns;

constexpr double tolerance = 1e-12;          // relative, for values larger than 1
constexpr double gregorian_year = 365.2425;  // days

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

void expect_close(double actual, double expected, const std::string& message) {
    check::expect_near(actual, expected, tolerance * std::max(1.0, std::fabs(expected)), message);
}

void expect_nan(double actual, const std::string& message) {
    check::expect(std::isnan(actual), message + " (expected NaN, got " + text_of(actual) + ')');
}

void expect_period(const DrawdownPeriod& actual, const DrawdownPeriod& expected, const std::string& name) {
    check::expect_equal(actual.peak, expected.peak, name + ": peak");
    check::expect_equal(actual.trough, expected.trough, name + ": trough");
    check::expect_equal(actual.recovery, expected.recovery, name + ": recovery");
    check::expect_equal(actual.recovered, expected.recovered, name + ": recovered");
    expect_close(actual.depth, expected.depth, name + ": depth");
    check::expect_equal(actual.from_initial, expected.from_initial, name + ": from the initial capital");
}

void expect_span(const DrawdownSpan& span, std::size_t first, std::size_t last, const std::string& name) {
    check::expect_equal(span.first, first, name + ": first bar");
    check::expect_equal(span.last, last, name + ": last bar");
}

Timestamp date(std::int64_t year, unsigned month, unsigned day) {
    return nanotrade::days_from_civil(year, month, day) * nanotrade::seconds_per_day;
}

// Only the timestamps matter to the metrics.
BarSeries bars_at(const std::vector<Timestamp>& times) {
    BarSeries bars;
    for (const Timestamp t : times) {
        bars.push_back(t, 100.0, 101.0, 99.0, 100.0, 1'000.0);
    }
    return bars;
}

// Monday to Friday from Monday 2024-01-01: bar k falls 7 (k / 5) + k % 5 days after it.
BarSeries weekday_bars(std::size_t count) {
    std::vector<Timestamp> times;
    for (std::size_t k = 0; k < count; ++k) {
        const auto days = static_cast<Timestamp>(7 * (k / 5) + k % 5);
        times.push_back(date(2024, 1, 1) + days * nanotrade::seconds_per_day);
    }
    return bars_at(times);
}

// Bars [begin, begin + equity.size()) at 252 a year, holding a position at every close.
BacktestResult result_of(std::vector<double> equity, double initial, std::size_t begin) {
    BacktestResult result;
    result.begin = begin;
    result.end = begin + equity.size();
    result.initial_capital = initial;
    result.bars_per_year = 252.0;
    result.position.assign(equity.size(), 100.0);
    result.exposure.assign(equity.size(), 1.0);
    result.equity = std::move(equity);
    return result;
}

std::vector<double> compound(double initial, const std::vector<double>& returns) {
    std::vector<double> equity;
    double value = initial;
    for (const double r : returns) {
        value *= 1.0 + r;
        equity.push_back(value);
    }
    return equity;
}

Trade closed_trade(std::size_t entry_bar, std::size_t exit_bar, double pnl, double return_pct) {
    Trade trade;
    trade.entry_bar = entry_bar;
    trade.exit_bar = exit_bar;
    trade.pnl = pnl;
    trade.return_pct = return_pct;
    trade.exit_reason = "exit";
    return trade;
}

// Still open when the window ended, valued at its last close.
Trade open_trade(std::size_t entry_bar, std::size_t exit_bar, double pnl, double return_pct) {
    Trade trade = closed_trade(entry_bar, exit_bar, pnl, return_pct);
    trade.exit_reason = "open";
    trade.open = true;
    return trade;
}

// ---- References, from the definitions in metrics.cpp ----

// Each bar's return, the first against the initial capital.
std::vector<double> returns_of(const BacktestResult& result) {
    std::vector<double> returns;
    double previous = result.initial_capital;
    for (const double value : result.equity) {
        returns.push_back(value / previous - 1.0);
        previous = value;
    }
    return returns;
}

double mean_of(const std::vector<double>& x) {
    double sum = 0.0;
    for (const double v : x) {
        sum += v;
    }
    return sum / static_cast<double>(x.size());
}

// The k-th moment about the mean, over n.
double central_moment(const std::vector<double>& x, double k) {
    const double mean = mean_of(x);
    double sum = 0.0;
    for (const double v : x) {
        sum += std::pow(v - mean, k);
    }
    return sum / static_cast<double>(x.size());
}

double sample_covariance(const std::vector<double>& x, const std::vector<double>& y) {
    const double mean_x = mean_of(x);
    const double mean_y = mean_of(y);
    double sum = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        sum += (x[i] - mean_x) * (y[i] - mean_y);
    }
    return sum / static_cast<double>(x.size() - 1);
}

double sample_stdev(const std::vector<double>& x) {
    return std::sqrt(sample_covariance(x, x));
}

double skewness_of(const std::vector<double>& x) {
    return central_moment(x, 3.0) / std::pow(central_moment(x, 2.0), 1.5);
}

double excess_kurtosis_of(const std::vector<double>& x) {
    return central_moment(x, 4.0) / std::pow(central_moment(x, 2.0), 2.0) - 3.0;
}

// The root mean square of the shortfalls below the per-bar risk-free rate, over every bar.
double downside_deviation(const std::vector<double>& returns, double risk_free) {
    double sum = 0.0;
    for (const double r : returns) {
        const double shortfall = std::min(r - risk_free, 0.0);
        sum += shortfall * shortfall;
    }
    return std::sqrt(sum / static_cast<double>(returns.size()));
}

// Type 7: between the order statistics on either side of p (n - 1), sorted from 0.
double quantile7(std::vector<double> x, double p) {
    std::sort(x.begin(), x.end());
    const double position = p * static_cast<double>(x.size() - 1);
    const auto below = static_cast<std::size_t>(std::floor(position));
    if (below + 1 >= x.size()) {
        return x[below];
    }
    return x[below] + (x[below + 1] - x[below]) * (position - static_cast<double>(below));
}

// The mean of the returns at or below the 5% quantile, as a loss.
double expected_shortfall(const std::vector<double>& returns) {
    const double threshold = quantile7(returns, 0.05);
    double sum = 0.0;
    double count = 0.0;
    for (const double r : returns) {
        if (r <= threshold) {
            sum += r;
            count += 1.0;
        }
    }
    return -sum / count;
}

double normal_cdf(double z) {
    return 0.5 * std::erfc(-z / std::sqrt(2.0));
}

// Bailey and Lopez de Prado: Phi((SR - SR*) sqrt(n - 1) / sqrt(1 - g3 SR + (g4 - 1) / 4 SR^2)), with g3 the skewness
// and g4 the plain kurtosis, three more than the excess.
double psr_of(double sharpe, double benchmark, std::size_t count, double skewness, double excess_kurtosis) {
    const double g4 = excess_kurtosis + 3.0;
    const double variance = 1.0 - skewness * sharpe + (g4 - 1.0) / 4.0 * sharpe * sharpe;
    return normal_cdf((sharpe - benchmark) * std::sqrt(static_cast<double>(count - 1)) / std::sqrt(variance));
}

// The runs of bars below zero in a drawdown series, deepest first: each from the last bar at the peak before it (the
// first bar, for a run from the initial capital before any bar reached it), through its deepest bar, to the first bar
// back at the peak.
std::vector<DrawdownPeriod> underwater_runs(const std::vector<double>& drawdown) {
    std::vector<DrawdownPeriod> runs;
    std::size_t high = 0;
    bool reached = false;  // whether any bar has been at the peak yet
    std::size_t i = 0;
    while (i < drawdown.size()) {
        if (!(drawdown[i] < 0.0)) {
            high = i;
            reached = true;
            ++i;
            continue;
        }
        DrawdownPeriod run{high, i, drawdown.size(), false, -drawdown[i], !reached};
        for (; i < drawdown.size() && drawdown[i] < 0.0; ++i) {
            if (-drawdown[i] > run.depth) {
                run.depth = -drawdown[i];
                run.trough = i;
            }
        }
        if (i < drawdown.size()) {
            run.recovery = i;
            run.recovered = true;
        }
        runs.push_back(run);
    }
    std::stable_sort(runs.begin(), runs.end(), [](const DrawdownPeriod& a, const DrawdownPeriod& b) {
        return a.depth > b.depth;
    });
    return runs;
}

// The bars a drawdown in the window [begin, end) spans, as indices into the series: from its peak, or from the close
// before the window when it fell from the initial capital (from the first bar, at the very start of the series), to
// its recovery, or to the window's last bar when it hasn't recovered.
DrawdownSpan span_of(const DrawdownPeriod& period, std::size_t begin, std::size_t end) {
    DrawdownSpan span;
    span.first = period.from_initial && begin > 0 ? begin - 1 : begin + period.peak;
    span.last = period.recovered ? begin + period.recovery : end - 1;
    return span;
}

// ---- A backtest by hand ----

struct Scenario {
    BarSeries bars;
    BacktestResult result;
};

constexpr double weekdays_per_year = 260.0;

// Twenty bars, Monday 2024-01-08 to Friday 2024-02-02, after five of warm-up, so the window counts from the close of
// Friday 2024-01-05, 28 days before its last. Long to bar 5, flat to bar 8, short from bar 9 to bar 15, flat on 16 and
// 17, and long from bar 18, still open at the end. Three drawdowns: from bar 1 to its recovery at bar 5; the longest,
// nine bars from the peak at bar 5 (Monday 2024-01-15) to the recovery at bar 14 (Friday 2024-01-26); and the deepest,
// from 103,300 at bar 14 to 98,500 at bar 15, recovered at bar 19.
Scenario main_scenario() {
    Scenario s{weekday_bars(30),
               result_of({101'000.0, 102'500.0, 101'800.0, 101'200.0, 102'000.0, 102'900.0, 102'400.0,
                          102'400.0, 102'400.0, 101'900.0, 102'100.0, 102'600.0, 102'300.0, 102'800.0,
                          103'300.0, 98'500.0,  101'000.0, 101'000.0, 102'500.0, 104'000.0},
                         100'000.0,
                         5)};
    s.result.bars_per_year = weekdays_per_year;
    s.result.position = {100.0,  100.0,  100.0,  100.0,  100.0,  100.0,  0.0, 0.0, 0.0,   -100.0,
                         -100.0, -100.0, -100.0, -100.0, -100.0, -100.0, 0.0, 0.0, 120.0, 120.0};
    // Trades count their bars from the start of the series.
    s.result.trades = {closed_trade(5, 11, 2'400.0, 0.024),
                       closed_trade(14, 21, -1'400.0, -0.014),
                       open_trade(23, 24, 3'000.0, 0.03)};
    s.result.commissions = 123.5;
    s.result.slippage = 45.25;
    s.result.borrow = 12.0;
    s.result.traded_value = 512'000.0;
    return s;
}

std::vector<double> main_benchmark() {
    return {0.006, 0.011, -0.004, -0.007, 0.009, 0.008,  -0.002, 0.001, -0.003, -0.006,
            0.004, 0.006, -0.001, 0.005,  0.004, -0.021, 0.013,  0.002, 0.010,  0.012};
}

// ---- The pieces ----

void bar_returns_start_from_the_initial_capital() {
    BacktestResult result = result_of({1'100.0, 990.0, 990.0, 1'089.0}, 1'000.0, 3);
    const std::vector<double> returns = nanotrade::bar_returns(result);
    check::expect_equal(returns.size(), std::size_t{4}, "one a bar");
    expect_close(returns[0], 0.1, "the first against the initial capital");
    expect_close(returns[1], -0.1, "the second against the first");
    check::expect_equal(returns[2], 0.0, "unchanged");
    expect_close(returns[3], 0.1, "the fourth against the third");
    result.equity.clear();
    check::expect(nanotrade::bar_returns(result).empty(), "no bars, no returns");
}

void moments_of_known_data() {
    // Mean 5; the deviations squared sum to 32, cubed to 42, and raised to the fourth power to 356.
    const std::vector<double> data = {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    const ReturnMoments m = nanotrade::moments(data);
    check::expect_equal(m.count, std::size_t{8}, "count");
    expect_close(m.mean, 5.0, "mean");
    expect_close(m.stdev, std::sqrt(32.0 / 7.0), "sample standard deviation");
    expect_close(m.skewness, (42.0 / 8.0) / std::pow(32.0 / 8.0, 1.5), "population skewness, 0.65625");
    expect_close(m.kurtosis, (356.0 / 8.0) / std::pow(32.0 / 8.0, 2.0) - 3.0, "population excess kurtosis, -0.21875");

    // Reordered and negated: the same but for the sign of the mean and the skewness.
    const std::vector<double> mirrored = {-9.0, -4.0, -5.0, -2.0, -7.0, -4.0, -5.0, -4.0};
    const ReturnMoments r = nanotrade::moments(mirrored);
    expect_close(r.mean, -5.0, "mirrored mean");
    expect_close(r.stdev, std::sqrt(32.0 / 7.0), "mirrored standard deviation");
    expect_close(r.skewness, -0.65625, "mirrored skewness");
    expect_close(r.kurtosis, -0.21875, "mirrored kurtosis");

    // One success in four, a Bernoulli variable with p = 1/4 and pq = 3/16: skewness (1 - 2p) / sqrt(pq), excess
    // kurtosis (1 - 6pq) / pq.
    const std::vector<double> bernoulli = {0.0, 0.0, 1.0, 0.0};
    const ReturnMoments b = nanotrade::moments(bernoulli);
    check::expect_equal(b.count, std::size_t{4}, "bernoulli count");
    expect_close(b.mean, 0.25, "bernoulli mean");
    expect_close(b.stdev, 0.5, "bernoulli sample standard deviation, sqrt(0.75 / 3)");
    expect_close(b.skewness, 0.5 / std::sqrt(0.1875), "bernoulli skewness");
    expect_close(b.kurtosis, (1.0 - 6.0 * 0.1875) / 0.1875, "bernoulli excess kurtosis");
}

void moments_of_few_or_equal_returns() {
    const ReturnMoments none = nanotrade::moments({});
    check::expect_equal(none.count, std::size_t{0}, "no returns");
    check::expect_equal(none.mean, 0.0, "no returns, no mean");
    check::expect_equal(none.stdev, 0.0, "no returns, no deviation");
    check::expect_equal(none.skewness, 0.0, "no returns, no skewness");
    check::expect_equal(none.kurtosis, 0.0, "no returns, no kurtosis");

    const std::vector<double> single = {0.03};
    const ReturnMoments one = nanotrade::moments(single);
    check::expect_equal(one.count, std::size_t{1}, "one return");
    check::expect_equal(one.mean, 0.03, "its mean");
    check::expect_equal(one.stdev, 0.0, "a sample deviation needs two");
    check::expect_equal(one.skewness, 0.0, "one return, no skewness");
    check::expect_equal(one.kurtosis, 0.0, "one return, no kurtosis");

    const std::vector<double> equal = {0.25, 0.25, 0.25};
    const ReturnMoments same = nanotrade::moments(equal);
    check::expect_equal(same.mean, 0.25, "equal returns' mean");
    check::expect_equal(same.stdev, 0.0, "equal returns don't deviate");
    check::expect_equal(same.skewness, 0.0, "nor skew without variance");
    check::expect_equal(same.kurtosis, 0.0, "nor have kurtosis");
}

void probabilistic_sharpe_by_its_formula() {
    // Normal returns, no skewness or excess kurtosis: Phi(0.1 sqrt(100) / sqrt(1 + 0.1^2 / 2)).
    expect_close(nanotrade::probabilistic_sharpe(0.1, 0.0, 101, 0.0, 0.0), 0.8407413278013518, "normal returns");
    // Left skew and fat tails widen the error: Phi(0.08 sqrt(250) / sqrt(1 + 0.5 * 0.1 + (3 + 2) / 4 * 0.1^2)).
    expect_close(nanotrade::probabilistic_sharpe(0.1, 0.02, 251, -0.5, 3.0), 0.8901157602489194, "skewed, fat tails");
    expect_close(nanotrade::probabilistic_sharpe(0.07, 0.07, 40, 0.8, 4.0), 0.5, "at the benchmark, even odds");

    struct Case {
        double sharpe;
        double benchmark;
        std::size_t count;
        double skewness;
        double kurtosis;
    };
    const std::vector<Case> cases = {{0.05, 0.0, 252, 0.0, 0.0},
                                     {-0.04, 0.0, 500, -1.2, 7.5},
                                     {0.3, 0.1, 12, 0.6, -1.0},
                                     {0.12, -0.05, 1'000, -0.3, 2.0},
                                     {0.0, 0.02, 60, 2.0, 10.0},
                                     {0.5, 0.0, 3, -2.0, 1.0}};
    for (const Case& c : cases) {
        expect_close(nanotrade::probabilistic_sharpe(c.sharpe, c.benchmark, c.count, c.skewness, c.kurtosis),
                     psr_of(c.sharpe, c.benchmark, c.count, c.skewness, c.kurtosis),
                     "sharpe " + text_of(c.sharpe) + " from " + std::to_string(c.count) + " returns");
    }
}

void probabilistic_sharpe_undefined() {
    expect_nan(nanotrade::probabilistic_sharpe(0.1, 0.0, 0, 0.0, 0.0), "no returns");
    expect_nan(nanotrade::probabilistic_sharpe(0.1, 0.0, 1, 0.0, 0.0), "one return");
    expect_close(nanotrade::probabilistic_sharpe(0.1, 0.0, 2, 0.0, 0.0), psr_of(0.1, 0.0, 2, 0.0, 0.0), "two will do");
    expect_nan(nanotrade::probabilistic_sharpe(std::numeric_limits<double>::quiet_NaN(), 0.0, 100, 0.0, 0.0),
               "no Sharpe ratio");
    expect_nan(nanotrade::probabilistic_sharpe(std::numeric_limits<double>::infinity(), 0.0, 100, 0.0, 0.0),
               "an infinite Sharpe ratio");
    expect_nan(nanotrade::probabilistic_sharpe(1.0, 0.0, 100, 1.0, -2.0), "no variance: 1 - 1 + 0");
    expect_nan(nanotrade::probabilistic_sharpe(2.0, 0.0, 100, 1.0, -2.0), "a negative variance: 1 - 2 + 0");
}

void drawdowns_from_the_running_peak() {
    // From the initial 100: 90 is 10% down; 99 after the peak of 110 too, and 108.9 after 121.
    const std::vector<double> equity = {90.0, 100.0, 110.0, 99.0, 121.0, 108.9, 121.0, 125.0};
    const std::vector<double> expected = {-0.1, 0.0, 0.0, -0.1, 0.0, -0.1, 0.0, 0.0};
    const std::vector<double> drawdown = nanotrade::drawdown_series(equity, 100.0);
    check::expect_equal(drawdown.size(), expected.size(), "one a bar");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expect_close(drawdown[i], expected[i], "bar " + std::to_string(i));
    }
    check::expect(nanotrade::drawdown_series({}, 100.0).empty(), "no bars, no drawdowns");
}

void worst_drawdowns_by_hand() {
    // From the initial 100 down to 95 at bar 0, back at bar 1; from 120 at bar 2 down to 84 at bar 4, back to 120 at
    // bar 6; from 130 at bar 7 to 117 at bar 8, back at bar 9; and from 140 at bar 10 to 105 at bar 12, still below at
    // the end of the 14 bars.
    const std::vector<double> equity =
        {95.0, 100.0, 120.0, 90.0, 84.0, 108.0, 120.0, 130.0, 117.0, 130.0, 140.0, 126.0, 105.0, 112.0};
    const DrawdownPeriod from_120{2, 4, 6, true, 0.3};
    const DrawdownPeriod from_140{10, 12, 14, false, 0.25};  // recovery: the window's length
    const DrawdownPeriod from_130{7, 8, 9, true, 0.1};
    const DrawdownPeriod from_100{0, 0, 1, true, 0.05, true};  // from the initial capital, so its peak is bar 0

    const std::vector<DrawdownPeriod> all = nanotrade::worst_drawdowns(equity, 100.0, 10);
    check::expect_equal(all.size(), std::size_t{4}, "four drawdowns");
    expect_period(all[0], from_120, "the deepest, 120 to 84");
    expect_period(all[1], from_140, "140 to 105, not recovered");
    expect_period(all[2], from_130, "130 to 117");
    expect_period(all[3], from_100, "100 to 95, from the initial capital");

    const std::vector<DrawdownPeriod> two = nanotrade::worst_drawdowns(equity, 100.0, 2);
    check::expect_equal(two.size(), std::size_t{2}, "the two worst");
    expect_period(two[0], from_120, "the worst");
    expect_period(two[1], from_140, "the second worst");

    check::expect(nanotrade::worst_drawdowns(equity, 100.0, 0).empty(), "none asked for");
    const std::vector<double> rising = {100.0, 101.0, 101.0, 105.0};
    check::expect(nanotrade::worst_drawdowns(rising, 100.0, 5).empty(), "never below the peak");
    check::expect(nanotrade::worst_drawdowns({}, 100.0, 5).empty(), "no bars");
}

void drawdowns_from_the_initial_capital() {
    // Below the initial 100 at the first close: it fell from the initial capital, before any close reached it.
    const std::vector<double> below = {99.0, 100.0, 100.0};
    const std::vector<DrawdownPeriod> a = nanotrade::worst_drawdowns(below, 100.0, 5);
    check::expect_equal(a.size(), std::size_t{1}, "one drawdown from below");
    expect_period(a[0], {0, 0, 1, true, 0.01, true}, "99 at the first close");

    // At the initial capital at the first close, then below: it fell from that close.
    const std::vector<double> at = {100.0, 99.0, 100.0};
    const std::vector<DrawdownPeriod> b = nanotrade::worst_drawdowns(at, 100.0, 5);
    check::expect_equal(b.size(), std::size_t{1}, "one drawdown from the first close");
    expect_period(b[0], {0, 1, 2, true, 0.01, false}, "100 at the first close, then 99");

    // Never back: from the initial capital and not recovered.
    const std::vector<double> never = {99.0, 98.0, 97.0};
    const std::vector<DrawdownPeriod> c = nanotrade::worst_drawdowns(never, 100.0, 5);
    check::expect_equal(c.size(), std::size_t{1}, "one drawdown, never recovered");
    expect_period(c[0], {0, 2, 3, false, 0.03, true}, "99, 98, 97");

    // Once a close has reached the initial capital, later drawdowns fall from closes.
    const std::vector<double> twice = {99.0, 101.0, 100.0};
    const std::vector<DrawdownPeriod> d = nanotrade::worst_drawdowns(twice, 100.0, 5);
    check::expect_equal(d.size(), std::size_t{2}, "two drawdowns");
    expect_period(d[0], {0, 0, 1, true, 0.01, true}, "99 from the initial 100");
    expect_period(d[1], {1, 2, 3, false, 1.0 - 100.0 / 101.0, false}, "100 from 101");
}

void the_bars_a_drawdown_spans() {
    // A window of fourteen bars, [10, 24). Recovered from a peak in the window: from the peak to the recovery.
    expect_span(nanotrade::drawdown_span({2, 4, 6, true, 0.3, false}, 10, 24), 12, 16, "peak to recovery");
    // A peak at the window's first close, which had reached the initial capital: from that close.
    expect_span(nanotrade::drawdown_span({0, 1, 2, true, 0.01, false}, 10, 24), 10, 12, "from the first close");
    // From the initial capital: from the close before the window, where CAGR starts counting.
    expect_span(nanotrade::drawdown_span({0, 0, 1, true, 0.05, true}, 10, 24), 9, 11, "from the close before");
    // At the very start of the series there's no close before the window, so from its first bar, as CAGR counts.
    expect_span(nanotrade::drawdown_span({0, 0, 1, true, 0.05, true}, 0, 14), 0, 1, "from the first bar at the start");
    // Not recovered: to the window's last bar, not one past it.
    expect_span(nanotrade::drawdown_span({10, 12, 14, false, 0.25, false}, 10, 24), 20, 23, "to the last bar");
    expect_span(nanotrade::drawdown_span({10, 12, 14, false, 0.25, false}, 0, 14),
                10,
                13,
                "to the last bar at the start");
    // Neither: the whole window, from the close before it, or from its first bar at the start of the series.
    expect_span(nanotrade::drawdown_span({0, 3, 14, false, 0.2, true}, 10, 24), 9, 23, "the close before to the last");
    expect_span(nanotrade::drawdown_span({0, 3, 14, false, 0.2, true}, 0, 14), 0, 13, "the first bar to the last");
}

// ---- compute_metrics ----

void total_return_years_and_cagr() {
    const Scenario s = main_scenario();
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    check::expect_equal(m.final_equity, 104'000.0, "final equity, the last close's");
    expect_close(m.total_return, 0.04, "total return");
    // From Friday 2024-01-05, the close before the window, to Friday 2024-02-02.
    const double years = 28.0 / gregorian_year;
    expect_close(m.years, years, "years from the calendar");
    expect_close(m.cagr, std::pow(104'000.0 / 100'000.0, 1.0 / years) - 1.0, "cagr");
}

void years_at_the_start_and_for_one_bar() {
    // At the start of the series there's no close before the window: its years run from the first bar, Monday
    // 2024-01-01, to the last, Friday 2024-01-12.
    const BacktestResult start = result_of(
        {100'000.0, 100'400.0, 100'900.0, 100'100.0, 101'300.0, 101'800.0, 101'200.0, 102'100.0, 102'600.0, 103'000.0},
        100'000.0,
        0);
    const Metrics m = nanotrade::compute_metrics(weekday_bars(10), start, {}, 0.0);
    const double years = 11.0 / gregorian_year;
    expect_close(m.years, years, "years from the first bar");
    expect_close(m.cagr, std::pow(103'000.0 / 100'000.0, 1.0 / years) - 1.0, "cagr from the first bar");

    // A single bar after the start counts from the close before it, over a weekend.
    const BacktestResult monday = result_of({101'000.0}, 100'000.0, 1);
    const Metrics weekend = nanotrade::compute_metrics(bars_at({date(2024, 1, 5), date(2024, 1, 8)}), monday, {}, 0.0);
    expect_close(weekend.years, 3.0 / gregorian_year, "Friday to Monday");

    // A single bar at the start spans no time, so its years come from the bars per year: a day of crypto.
    BacktestResult first = result_of({101'000.0}, 100'000.0, 0);
    first.bars_per_year = 365.0;
    first.traded_value = 101'000.0;
    const Metrics one = nanotrade::compute_metrics(bars_at({date(2024, 3, 2)}), first, {}, 0.0);
    expect_close(one.years, 1.0 / 365.0, "a day");
    expect_close(one.cagr, std::pow(1.01, 365.0) - 1.0, "1% a day for a year");
    expect_close(one.turnover, 365.0, "the equity traded once a day");
    check::expect_equal(one.volatility, 0.0, "one return doesn't deviate");
    expect_nan(one.sharpe, "one return has no Sharpe ratio");
    expect_nan(one.psr, "nor a probabilistic one");
}

void volatility_sharpe_and_sortino() {
    const Scenario s = main_scenario();
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    const std::vector<double> returns = returns_of(s.result);
    const double mean = mean_of(returns);
    const double stdev = sample_stdev(returns);
    const double annualize = std::sqrt(weekdays_per_year);
    expect_close(m.volatility, stdev * annualize, "volatility: the sample standard deviation, annualized");
    expect_close(m.sharpe, mean / stdev * annualize, "sharpe");
    // Six bars of the twenty lose, but the downside deviation averages over all twenty.
    expect_close(m.sortino, mean / downside_deviation(returns, 0.0) * annualize, "sortino");
    const double skewness = skewness_of(returns);
    const double kurtosis = excess_kurtosis_of(returns);
    expect_close(m.skewness, skewness, "skewness");
    expect_close(m.kurtosis, kurtosis, "excess kurtosis");
    expect_close(m.psr, psr_of(mean / stdev, 0.0, returns.size(), skewness, kurtosis), "psr of the per-bar Sharpe");
}

void net_of_a_risk_free_rate() {
    const Scenario s = main_scenario();
    const std::vector<double> benchmark = main_benchmark();
    const Metrics gross = nanotrade::compute_metrics(s.bars, s.result, benchmark, 0.0);
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, benchmark, 0.05);
    const std::vector<double> returns = returns_of(s.result);
    const double risk_free = 0.05 / weekdays_per_year;
    const double excess = mean_of(returns) - risk_free;
    const double stdev = sample_stdev(returns);
    const double annualize = std::sqrt(weekdays_per_year);
    expect_close(m.sharpe, excess / stdev * annualize, "sharpe");
    // The three flat bars fall short of the risk-free rate too.
    expect_close(m.sortino, excess / downside_deviation(returns, risk_free) * annualize, "sortino");
    expect_close(m.psr,
                 psr_of(excess / stdev, 0.0, returns.size(), skewness_of(returns), excess_kurtosis_of(returns)),
                 "psr");
    check::expect(m.sharpe < gross.sharpe && m.sortino < gross.sortino, "both lower");
    // The rate is for those ratios alone.
    check::expect_equal(m.volatility, gross.volatility, "volatility");
    check::expect_equal(m.cagr, gross.cagr, "cagr");
    check::expect_equal(m.max_drawdown, gross.max_drawdown, "max drawdown");
    check::expect_equal(m.var_95, gross.var_95, "value at risk");
    check::expect_equal(m.beta, gross.beta, "beta");
    check::expect_equal(m.alpha, gross.alpha, "alpha");
    check::expect_equal(m.correlation, gross.correlation, "correlation");
}

void drawdown_metrics() {
    const Scenario s = main_scenario();
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    const double max_drawdown = 1.0 - 98'500.0 / 103'300.0;
    expect_close(m.max_drawdown, max_drawdown, "the deepest drawdown, 103,300 at bar 14 to 98,500 at bar 15");
    const double cagr = std::pow(104'000.0 / 100'000.0, gregorian_year / 28.0) - 1.0;
    expect_close(m.calmar, cagr / max_drawdown, "calmar: cagr over max drawdown");
    check::expect_equal(m.longest_drawdown_bars,
                        std::size_t{9},
                        "the longest drawdown isn't the deepest: bars 5 to 14");
    check::expect_equal(m.longest_drawdown_days, 11.0, "Monday 2024-01-15 to Friday 2024-01-26");
}

void an_unrecovered_drawdown_runs_to_the_end() {
    // From 1,010 at bar 0 to 1,000, back at bar 2; then from 1,030 at bar 3 down to 1,000, and still below at the last
    // of the 11 bars, bar 10, so it runs to that bar: 7 bars, and 9 days from the peak, Monday 2024-01-08, to the last
    // close, Wednesday 2024-01-17.
    const BacktestResult result =
        result_of({1'010.0, 1'000.0, 1'020.0, 1'030.0, 1'025.0, 1'010.0, 1'015.0, 1'000.0, 1'005.0, 1'020.0, 1'029.0},
                  1'000.0,
                  2);
    const Metrics m = nanotrade::compute_metrics(weekday_bars(20), result, {}, 0.0);
    check::expect_equal(m.longest_drawdown_bars, std::size_t{7}, "bars 3 to 10, the last");
    check::expect_equal(m.longest_drawdown_days, 9.0, "days to the last close");
    expect_close(m.max_drawdown, 1.0 - 1'000.0 / 1'030.0, "1,030 to 1,000");
}

void a_drawdown_from_the_initial_capital_counts_from_the_close_before() {
    // A window of Monday 2024-01-08 to Wednesday 2024-01-10 after a close on Friday 2024-01-05, from an initial 100.
    const BarSeries bars = bars_at({date(2024, 1, 5), date(2024, 1, 8), date(2024, 1, 9), date(2024, 1, 10)});

    // Below the initial capital at Monday's close and back on Tuesday: from Friday's close, where CAGR counts from.
    const Metrics below = nanotrade::compute_metrics(bars, result_of({99.0, 100.0, 100.0}, 100.0, 1), {}, 0.0);
    check::expect_equal(below.longest_drawdown_bars, std::size_t{2}, "Friday to Tuesday, in bars");
    check::expect_equal(below.longest_drawdown_days, 4.0, "Friday to Tuesday, in days");

    // At the initial capital on Monday, below on Tuesday, and back on Wednesday: from Monday's close.
    const Metrics at = nanotrade::compute_metrics(bars, result_of({100.0, 99.0, 100.0}, 100.0, 1), {}, 0.0);
    check::expect_equal(at.longest_drawdown_bars, std::size_t{2}, "Monday to Wednesday, in bars");
    check::expect_equal(at.longest_drawdown_days, 2.0, "Monday to Wednesday, in days");

    // Never back: from Friday's close to the last, Wednesday's.
    const Metrics never = nanotrade::compute_metrics(bars, result_of({99.0, 98.0, 97.0}, 100.0, 1), {}, 0.0);
    check::expect_equal(never.longest_drawdown_bars, std::size_t{3}, "Friday to Wednesday, in bars");
    check::expect_equal(never.longest_drawdown_days, 5.0, "Friday to Wednesday, in days");

    // At the very start of the series there's no close before the window: from its first bar, as CAGR counts.
    const Metrics start = nanotrade::compute_metrics(bars, result_of({99.0, 100.0, 100.0, 100.0}, 100.0, 0), {}, 0.0);
    check::expect_equal(start.longest_drawdown_bars, std::size_t{1}, "Friday to Monday, in bars");
    check::expect_equal(start.longest_drawdown_days, 3.0, "Friday to Monday, in days");
}

void exposure_turnover_and_costs() {
    const Scenario s = main_scenario();
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    check::expect_equal(m.exposure, 0.75, "15 of 20 bars hold a position, long or short");
    const double years = 28.0 / gregorian_year;
    expect_close(m.turnover, 512'000.0 / mean_of(s.result.equity) / years, "traded value over average equity, a year");
    expect_close(m.costs, 180.75, "commissions, slippage, and borrowing");
}

void value_at_risk() {
    // Thirty returns in no order. The 5% quantile sits at 0.05 * 29 = 1.45 among them sorted from 0, 0.45 of the way
    // from the second smallest, -3.1%, to the third, -2.2%: -2.695%. The two at or below it average -3.55%.
    const std::vector<double> returns = {0.004,  -0.031, 0.012,  0.007,  -0.006, 0.015,  0.002,  -0.040, 0.009,  0.001,
                                         -0.003, 0.018,  0.005,  -0.022, 0.011,  0.003,  -0.009, 0.006,  0.014,  -0.001,
                                         0.008,  0.010,  -0.005, 0.016,  0.000,  -0.008, 0.013,  0.004,  -0.002, 0.020};
    const BacktestResult result = result_of(compound(10'000.0, returns), 10'000.0, 5);
    const Metrics m = nanotrade::compute_metrics(weekday_bars(40), result, {}, 0.0);
    expect_close(m.var_95, 0.02695, "value at risk");
    expect_close(m.cvar_95, 0.0355, "expected shortfall");

    // Twenty: 0.95 of the way from the smallest to the next, which leaves the smallest alone in the tail.
    const Scenario s = main_scenario();
    const Metrics twenty = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    const std::vector<double> main_returns = returns_of(s.result);
    expect_close(twenty.var_95, -quantile7(main_returns, 0.05), "value at risk of twenty");
    expect_close(twenty.cvar_95, expected_shortfall(main_returns), "expected shortfall of twenty");
    expect_close(twenty.cvar_95, 1.0 - 98'500.0 / 103'300.0, "the worst bar alone");
}

void trade_statistics() {
    Scenario s = main_scenario();
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    check::expect_equal(m.trades, std::size_t{2}, "two closed trades and an open one");
    check::expect_equal(m.win_rate, 0.5, "one win in two");
    expect_close(m.profit_factor, 2'400.0 / 1'400.0, "profit factor");
    check::expect_equal(m.best_trade, 0.024, "the open trade's 3% isn't the best");
    expect_close(m.avg_bars_held, 6.5, "6 and 7 bars held");

    // Two wins and five losses, closed, then an open loss that would be the worst trade and a fourth loss in a row.
    s.result.trades = {closed_trade(5, 8, 2'400.0, 0.024),
                       closed_trade(8, 10, -300.0, -0.003),
                       closed_trade(10, 11, -500.0, -0.005),
                       closed_trade(11, 14, 1'000.0, 0.010),
                       closed_trade(14, 16, -200.0, -0.002),
                       closed_trade(16, 19, -700.0, -0.007),
                       closed_trade(19, 20, -100.0, -0.001),
                       open_trade(21, 24, -9'000.0, -0.09)};
    const Metrics seven = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    check::expect_equal(seven.trades, std::size_t{7}, "closed trades");
    expect_close(seven.win_rate, 2.0 / 7.0, "win rate");
    expect_close(seven.profit_factor, 3'400.0 / 1'800.0, "profit factor: gross profit over gross loss");
    expect_close(seven.avg_trade, 0.016 / 7.0, "average trade return");
    expect_close(seven.avg_win, 0.017, "average win");
    expect_close(seven.avg_loss, -0.0036, "average loss");
    check::expect_equal(seven.best_trade, 0.024, "best trade");
    check::expect_equal(seven.worst_trade, -0.007, "worst trade");
    expect_close(seven.avg_bars_held, 15.0 / 7.0, "average bars held");
    check::expect_equal(seven.max_losing_streak, std::size_t{3}, "longest losing streak");

    // An open win counts no more than an open loss.
    s.result.trades.back() = open_trade(21, 24, 9'000.0, 0.09);
    const Metrics open_win = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    check::expect_equal(open_win.trades, std::size_t{7}, "closed trades, with an open win");
    expect_close(open_win.win_rate, 2.0 / 7.0, "win rate, with an open win");
    expect_close(open_win.profit_factor, 3'400.0 / 1'800.0, "profit factor, with an open win");
    check::expect_equal(open_win.best_trade, 0.024, "best trade, with an open win");
}

void expect_no_trade_statistics(const Metrics& m, const std::string& name) {
    check::expect_equal(m.trades, std::size_t{0}, name + ": trades");
    expect_nan(m.win_rate, name + ": win rate");
    expect_nan(m.profit_factor, name + ": profit factor");
    expect_nan(m.avg_trade, name + ": average trade");
    expect_nan(m.avg_win, name + ": average win");
    expect_nan(m.avg_loss, name + ": average loss");
    expect_nan(m.best_trade, name + ": best trade");
    expect_nan(m.worst_trade, name + ": worst trade");
    expect_nan(m.avg_bars_held, name + ": bars held");
    check::expect_equal(m.max_losing_streak, std::size_t{0}, name + ": losing streak");
}

void trade_ratios_without_trades() {
    Scenario s = main_scenario();
    s.result.trades.clear();
    expect_no_trade_statistics(nanotrade::compute_metrics(s.bars, s.result, {}, 0.0), "no trades");
    s.result.trades = {open_trade(23, 24, -3'000.0, -0.03)};
    expect_no_trade_statistics(nanotrade::compute_metrics(s.bars, s.result, {}, 0.0), "only an open trade");
}

void no_losing_trades() {
    Scenario s = main_scenario();
    s.result.trades = {closed_trade(5, 11, 2'400.0, 0.024),
                       closed_trade(14, 21, 1'000.0, 0.01),
                       open_trade(23, 24, -3'000.0, -0.03)};
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, {}, 0.0);
    check::expect_equal(m.trades, std::size_t{2}, "two closed trades");
    check::expect_equal(m.win_rate, 1.0, "both won");
    expect_nan(m.profit_factor, "no gross loss to divide by");
    expect_nan(m.avg_loss, "no losses to average");
    expect_close(m.avg_win, 0.017, "average win");
    expect_close(m.avg_trade, 0.017, "average trade");
    check::expect_equal(m.worst_trade, 0.01, "the worst trade is a win");
    check::expect_equal(m.max_losing_streak, std::size_t{0}, "no losing streak");
}

void ratios_without_volatility() {
    // Never in the market: the equity never moves.
    BacktestResult flat = result_of(std::vector<double>(10, 50'000.0), 50'000.0, 5);
    flat.position.assign(10, 0.0);
    const std::vector<double> benchmark = {0.01, -0.02, 0.005, 0.0, 0.012, -0.007, 0.003, 0.009, -0.011, 0.004};
    const Metrics m = nanotrade::compute_metrics(weekday_bars(20), flat, benchmark, 0.0);
    check::expect_equal(m.total_return, 0.0, "total return");
    expect_close(m.cagr, 0.0, "cagr");
    check::expect_equal(m.volatility, 0.0, "volatility");
    expect_nan(m.sharpe, "sharpe");
    expect_nan(m.sortino, "sortino");
    expect_nan(m.psr, "psr");
    check::expect_equal(m.skewness, 0.0, "skewness");
    check::expect_equal(m.kurtosis, 0.0, "kurtosis");
    check::expect_equal(m.max_drawdown, 0.0, "max drawdown");
    expect_nan(m.calmar, "calmar");
    check::expect_equal(m.longest_drawdown_bars, std::size_t{0}, "longest drawdown");
    check::expect_equal(m.longest_drawdown_days, 0.0, "longest drawdown in days");
    check::expect_equal(m.exposure, 0.0, "exposure");
    check::expect_equal(m.turnover, 0.0, "turnover");
    expect_close(m.var_95, 0.0, "value at risk");
    expect_close(m.cvar_95, 0.0, "expected shortfall");
    check::expect_equal(m.beta, 0.0, "a flat curve doesn't move with the benchmark");
    check::expect_equal(m.alpha, 0.0, "nor beat it");
    expect_nan(m.correlation, "correlation");

    // Steady growth, 25% every bar exactly: no deviation, no downside, and no drawdown.
    const BacktestResult steady = result_of({125.0, 156.25, 195.3125, 244.140625}, 100.0, 5);
    const Metrics g = nanotrade::compute_metrics(weekday_bars(20), steady, {}, 0.0);
    check::expect_equal(g.volatility, 0.0, "steady volatility");
    expect_nan(g.sharpe, "steady sharpe");
    expect_nan(g.sortino, "steady sortino");
    expect_nan(g.psr, "steady psr");
    check::expect_equal(g.max_drawdown, 0.0, "steady max drawdown");
    expect_nan(g.calmar, "steady calmar");
}

void beta_alpha_and_correlation() {
    const std::vector<double> benchmark = main_benchmark();
    const BarSeries bars = weekday_bars(30);

    // Half the benchmark's return plus 0.04% a bar: beta 0.5, alpha 0.04% a bar for 252 bars, correlation 1. And
    // minus one and a half times the benchmark's less 0.02% a bar.
    std::vector<double> half;
    std::vector<double> mirror;
    for (const double b : benchmark) {
        half.push_back(0.5 * b + 0.0004);
        mirror.push_back(-1.5 * b - 0.0002);
    }
    const Metrics h = nanotrade::compute_metrics(bars, result_of(compound(1e5, half), 1e5, 5), benchmark, 0.0);
    expect_close(h.beta, 0.5, "beta of half the benchmark");
    expect_close(h.alpha, 0.0004 * 252.0, "alpha of half the benchmark");
    expect_close(h.correlation, 1.0, "correlation of half the benchmark");
    const Metrics r = nanotrade::compute_metrics(bars, result_of(compound(1e5, mirror), 1e5, 5), benchmark, 0.0);
    expect_close(r.beta, -1.5, "beta of the mirror image");
    expect_close(r.alpha, -0.0002 * 252.0, "alpha of the mirror image");
    expect_close(r.correlation, -1.0, "correlation of the mirror image");

    // In general, from the sample covariance.
    const Scenario s = main_scenario();
    const Metrics m = nanotrade::compute_metrics(s.bars, s.result, benchmark, 0.0);
    const std::vector<double> returns = returns_of(s.result);
    const double covariance = sample_covariance(returns, benchmark);
    const double expected_beta = covariance / sample_covariance(benchmark, benchmark);
    expect_close(m.beta, expected_beta, "beta");
    expect_close(m.alpha, (mean_of(returns) - expected_beta * mean_of(benchmark)) * weekdays_per_year, "alpha");
    expect_close(m.correlation, covariance / (sample_stdev(returns) * sample_stdev(benchmark)), "correlation");
}

void expect_no_benchmark_statistics(const Metrics& m, const std::string& name) {
    expect_nan(m.beta, name + ": beta");
    expect_nan(m.alpha, name + ": alpha");
    expect_nan(m.correlation, name + ": correlation");
}

void benchmark_statistics_undefined() {
    const Scenario s = main_scenario();
    std::vector<double> shorter = main_benchmark();
    shorter.pop_back();
    const std::vector<double> flat(20, 0.0);
    expect_no_benchmark_statistics(nanotrade::compute_metrics(s.bars, s.result, {}, 0.0), "no benchmark");
    expect_no_benchmark_statistics(nanotrade::compute_metrics(s.bars, s.result, shorter, 0.0), "one return short");
    expect_no_benchmark_statistics(nanotrade::compute_metrics(s.bars, s.result, flat, 0.0), "a flat benchmark");
    const std::vector<double> single = {0.01};
    const BacktestResult one = result_of({101'000.0}, 100'000.0, 5);
    expect_no_benchmark_statistics(nanotrade::compute_metrics(weekday_bars(10), one, single, 0.0), "a single bar");
}

void a_ruined_account() {
    BacktestResult result = result_of({600.0, 0.0, 0.0}, 1'000.0, 5);
    result.position = {10.0, 0.0, 0.0};
    result.ruined = true;
    const std::vector<double> returns = nanotrade::bar_returns(result);
    check::expect_equal(returns.size(), std::size_t{3}, "one a bar");
    expect_close(returns[0], -0.4, "down 40%");
    check::expect_equal(returns[1], -1.0, "then everything");
    check::expect_equal(returns[2], 0.0, "and nothing left to lose");
    const Metrics m = nanotrade::compute_metrics(weekday_bars(10), result, {}, 0.0);
    check::expect_equal(m.final_equity, 0.0, "final equity");
    check::expect_equal(m.total_return, -1.0, "total return");
    check::expect_equal(m.cagr, -1.0, "cagr");
    check::expect_equal(m.max_drawdown, 1.0, "max drawdown");
}

// ---- The calendar ----

void calendar_returns_by_month_and_year() {
    // A window from Tuesday 2023-11-14 to Monday 2024-03-04, between closes on 2023-10-31 and 2024-03-05.
    const BarSeries bars = bars_at({date(2023, 10, 31),
                                    date(2023, 11, 14),
                                    date(2023, 11, 22),
                                    date(2023, 11, 30),
                                    date(2023, 12, 1),
                                    date(2023, 12, 15),
                                    date(2023, 12, 29),
                                    date(2024, 1, 2),
                                    date(2024, 1, 31),
                                    date(2024, 2, 1),
                                    date(2024, 2, 29),
                                    date(2024, 3, 4),
                                    date(2024, 3, 5)});
    const BacktestResult result =
        result_of({1'010.0, 990.0, 1'020.0, 1'030.0, 1'005.0, 1'050.0, 1'040.0, 1'100.0, 1'080.0, 1'120.0, 1'150.0},
                  1'000.0,
                  1);
    const std::vector<YearReturns> years = nanotrade::calendar_returns(bars, result);
    check::expect_equal(years.size(), std::size_t{2}, "2023 and 2024");

    check::expect_equal(years[0].year, std::int64_t{2023}, "first year");
    for (std::size_t month = 0; month < 10; ++month) {
        expect_nan(years[0].months[month], "2023 month " + std::to_string(month + 1) + ", before the window");
    }
    expect_close(years[0].months[10], 1'020.0 / 1'000.0 - 1.0, "November 2023, from the initial capital");
    expect_close(years[0].months[11], 1'050.0 / 1'020.0 - 1.0, "December 2023");
    expect_close(years[0].total, 1'050.0 / 1'000.0 - 1.0, "2023");

    check::expect_equal(years[1].year, std::int64_t{2024}, "second year");
    expect_close(years[1].months[0], 1'100.0 / 1'050.0 - 1.0, "January 2024");
    expect_close(years[1].months[1], 1'120.0 / 1'100.0 - 1.0, "February 2024, to the leap day");
    expect_close(years[1].months[2], 1'150.0 / 1'120.0 - 1.0, "March 2024, to the last bar");
    for (std::size_t month = 3; month < 12; ++month) {
        expect_nan(years[1].months[month], "2024 month " + std::to_string(month + 1) + ", after the window");
    }
    expect_close(years[1].total, 1'150.0 / 1'050.0 - 1.0, "2024");

    for (const YearReturns& row : years) {
        double growth = 1.0;
        for (const double month : row.months) {
            if (!std::isnan(month)) {
                growth *= 1.0 + month;
            }
        }
        expect_close(row.total, growth - 1.0, std::to_string(row.year) + " compounds its months");
    }
}

void expect_only_month(const YearReturns& row, std::size_t month, double expected, const std::string& name) {
    for (std::size_t m = 0; m < 12; ++m) {
        if (m == month) {
            expect_close(row.months[m], expected, name);
        } else {
            expect_nan(row.months[m], name + ", month " + std::to_string(m + 1));
        }
    }
}

void calendar_returns_of_monthly_and_yearly_bars() {
    // A bar at the end of each month from the start of the series: each its own month, the first against the initial
    // capital.
    const BarSeries monthly = bars_at({date(2022, 11, 30), date(2022, 12, 30), date(2023, 1, 31), date(2023, 2, 28)});
    const std::vector<YearReturns> months =
        nanotrade::calendar_returns(monthly, result_of({1'050.0, 1'029.0, 1'100.0, 1'078.0}, 1'000.0, 0));
    check::expect_equal(months.size(), std::size_t{2}, "2022 and 2023");
    check::expect_equal(months[0].year, std::int64_t{2022}, "2022");
    expect_close(months[0].months[10], 0.05, "November 2022");
    expect_close(months[0].months[11], 1'029.0 / 1'050.0 - 1.0, "December 2022");
    expect_close(months[0].total, 0.029, "2022");
    check::expect_equal(months[1].year, std::int64_t{2023}, "2023");
    expect_close(months[1].months[0], 1'100.0 / 1'029.0 - 1.0, "January 2023");
    expect_close(months[1].months[1], 1'078.0 / 1'100.0 - 1.0, "February 2023");
    expect_close(months[1].total, 1'078.0 / 1'029.0 - 1.0, "2023");

    // A bar a year, each in June: the month repeats, but the years differ.
    const BarSeries yearly = bars_at({date(2021, 6, 30), date(2022, 6, 30), date(2023, 6, 30)});
    const std::vector<YearReturns> years =
        nanotrade::calendar_returns(yearly, result_of({1'100.0, 990.0, 1'188.0}, 1'000.0, 0));
    check::expect_equal(years.size(), std::size_t{3}, "three years");
    const double june[] = {0.1, -0.1, 0.2};
    for (std::size_t k = 0; k < 3; ++k) {
        const std::string name = "June " + std::to_string(2021 + k);
        check::expect_equal(years[k].year, static_cast<std::int64_t>(2021 + k), name + "'s year");
        expect_only_month(years[k], 5, june[k], name);
        expect_close(years[k].total, june[k], name + ", the whole year");
    }
}

// ---- Random curves ----

// Random walks with flat stretches, over random windows of three years of weekday bars.
void random_curves_agree() {
    nanotrade::Random random(2024);
    const BarSeries bars = weekday_bars(800);
    for (int trial = 0; trial < 300; ++trial) {
        const auto begin = static_cast<std::size_t>(random.below(200));
        const auto length = static_cast<std::size_t>(1 + random.below(600));
        std::vector<double> equity;
        double value = 1'000.0;
        for (std::size_t i = 0; i < length; ++i) {
            if (random.uniform() < 0.8) {
                value *= 1.0 + 0.02 * random.normal();
            }
            equity.push_back(value);
        }
        const BacktestResult result = result_of(equity, 1'000.0, begin);
        const std::string label = "trial " + std::to_string(trial);

        // The drawdown periods are the runs below the peak in the drawdown series, deepest first.
        const std::vector<DrawdownPeriod> runs = underwater_runs(nanotrade::drawdown_series(equity, 1'000.0));
        const std::vector<DrawdownPeriod> periods = nanotrade::worst_drawdowns(equity, 1'000.0, length);
        check::expect_equal(periods.size(), runs.size(), label + ": a period for each run below the peak");
        for (std::size_t k = 0; k < runs.size(); ++k) {
            expect_period(periods[k], runs[k], label + ", period " + std::to_string(k));
        }
        const std::vector<DrawdownPeriod> worst = nanotrade::worst_drawdowns(equity, 1'000.0, 3);
        check::expect_equal(worst.size(), std::min(runs.size(), std::size_t{3}), label + ": the three worst");
        for (std::size_t k = 0; k < worst.size(); ++k) {
            expect_period(worst[k], runs[k], label + ", worst " + std::to_string(k));
        }

        const Metrics m = nanotrade::compute_metrics(bars, result, {}, 0.0);
        expect_close(m.max_drawdown, runs.empty() ? 0.0 : runs.front().depth, label + ": max drawdown");
        // Each period spans the bars the definition gives, and the longest drawdown's bars and days are its span's.
        std::size_t longest = 0;
        for (std::size_t k = 0; k < runs.size(); ++k) {
            const DrawdownSpan span = span_of(runs[k], begin, begin + length);
            expect_span(nanotrade::drawdown_span(periods[k], begin, begin + length),
                        span.first,
                        span.last,
                        label + ", span " + std::to_string(k));
            longest = std::max(longest, span.last - span.first);
        }
        check::expect_equal(m.longest_drawdown_bars, longest, label + ": longest drawdown");
        bool days_match = runs.empty() && m.longest_drawdown_days == 0.0;
        for (const DrawdownPeriod& run : runs) {
            const DrawdownSpan span = span_of(run, begin, begin + length);
            const double days = static_cast<double>(bars.time[span.last] - bars.time[span.first]) /
                                static_cast<double>(nanotrade::seconds_per_day);
            days_match = days_match || (span.last - span.first == longest && m.longest_drawdown_days == days);
        }
        check::expect(days_match, label + ": the longest drawdown's days");

        const std::vector<double> returns = returns_of(result);
        expect_close(m.total_return, equity.back() / 1'000.0 - 1.0, label + ": total return");
        expect_close(m.var_95, -quantile7(returns, 0.05), label + ": value at risk");
        expect_close(m.cvar_95, expected_shortfall(returns), label + ": expected shortfall");
        if (length > 1) {
            const double stdev = sample_stdev(returns);
            const double downside = downside_deviation(returns, 0.0);
            if (stdev > 0.0) {
                expect_close(m.volatility, stdev * std::sqrt(252.0), label + ": volatility");
                expect_close(m.sharpe, mean_of(returns) / stdev * std::sqrt(252.0), label + ": sharpe");
            }
            if (downside > 0.0) {
                expect_close(m.sortino, mean_of(returns) / downside * std::sqrt(252.0), label + ": sortino");
            }
        }

        // A row for each year of the window and a return for each of its months, compounding to the total return.
        const std::vector<YearReturns> calendar = nanotrade::calendar_returns(bars, result);
        const CivilDate first_date = nanotrade::civil_from_timestamp(bars.time[begin]);
        const CivilDate last_date = nanotrade::civil_from_timestamp(bars.time[begin + length - 1]);
        check::expect_equal(calendar.size(),
                            static_cast<std::size_t>(last_date.year - first_date.year + 1),
                            label + ": years");
        const std::int64_t months_spanned = 12 * (last_date.year - first_date.year) +
                                            static_cast<std::int64_t>(last_date.month) -
                                            static_cast<std::int64_t>(first_date.month) + 1;
        std::int64_t months_found = 0;
        double growth = 1.0;
        for (std::size_t k = 0; k < calendar.size(); ++k) {
            const YearReturns& row = calendar[k];
            check::expect_equal(row.year, first_date.year + static_cast<std::int64_t>(k), label + ": year in order");
            double year_growth = 1.0;
            for (const double month : row.months) {
                if (!std::isnan(month)) {
                    ++months_found;
                    year_growth *= 1.0 + month;
                }
            }
            expect_close(row.total, year_growth - 1.0, label + ": " + std::to_string(row.year) + " compounds");
            growth *= 1.0 + row.total;
        }
        check::expect_equal(months_found, months_spanned, label + ": a return for each month of the window");
        expect_close(growth - 1.0, m.total_return, label + ": the years compound to the total return");
    }
}

}  // namespace

int main() {
    return check::run({
        {"bar returns start from the initial capital", bar_returns_start_from_the_initial_capital},
        {"moments of known data", moments_of_known_data},
        {"moments of no returns, one, and equal ones", moments_of_few_or_equal_returns},
        {"the probabilistic Sharpe ratio by its formula", probabilistic_sharpe_by_its_formula},
        {"the probabilistic Sharpe ratio needs two returns and a variance", probabilistic_sharpe_undefined},
        {"drawdowns from the running peak", drawdowns_from_the_running_peak},
        {"the worst drawdowns by hand", worst_drawdowns_by_hand},
        {"drawdowns from the initial capital", drawdowns_from_the_initial_capital},
        {"the bars a drawdown spans", the_bars_a_drawdown_spans},
        {"total return, and CAGR over the years from the close before the window", total_return_years_and_cagr},
        {"years at the start of the series and for a single bar", years_at_the_start_and_for_one_bar},
        {"volatility, Sharpe, Sortino, and the moments of the returns", volatility_sharpe_and_sortino},
        {"Sharpe, Sortino, and PSR net of a risk-free rate, and nothing else", net_of_a_risk_free_rate},
        {"max drawdown, Calmar, and the longest drawdown in bars and days", drawdown_metrics},
        {"an unrecovered drawdown runs to the end of the window", an_unrecovered_drawdown_runs_to_the_end},
        {"a drawdown from the initial capital counts from the close before the window",
         a_drawdown_from_the_initial_capital_counts_from_the_close_before},
        {"exposure, turnover, and costs", exposure_turnover_and_costs},
        {"value at risk and expected shortfall at 95%", value_at_risk},
        {"trade statistics leave open trades out", trade_statistics},
        {"trade ratios without closed trades are NaN", trade_ratios_without_trades},
        {"without losing trades the profit factor is NaN", no_losing_trades},
        {"ratios without volatility or drawdowns are NaN", ratios_without_volatility},
        {"beta, alpha, and correlation against a benchmark", beta_alpha_and_correlation},
        {"benchmark statistics need a return a bar and some variance", benchmark_statistics_undefined},
        {"a ruined account", a_ruined_account},
        {"calendar returns by month and year", calendar_returns_by_month_and_year},
        {"calendar returns of monthly and yearly bars", calendar_returns_of_monthly_and_yearly_bars},
        {"random curves: drawdowns, metrics, and calendar agree", random_curves_agree},
    });
}
