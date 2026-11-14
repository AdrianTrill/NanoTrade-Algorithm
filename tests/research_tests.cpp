// The research tools: parameter sweeps against separate backtests and across thread counts, the probability of
// backtest overfitting and the deflated Sharpe ratio on returns whose answers are known, walk-forward folds and the
// equity stitched from them, the Monte Carlo bootstrap, and the synthetic markets.

#include "check.hpp"

#include "nanotrade/engine.hpp"
#include "nanotrade/error.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/metrics.hpp"
#include "nanotrade/random.hpp"
#include "nanotrade/research.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/strategy.hpp"
#include "nanotrade/synthetic.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using nanotrade::BacktestConfig;
using nanotrade::BacktestResult;
using nanotrade::BarSeries;
using nanotrade::Metrics;
using nanotrade::MonteCarloResult;
using nanotrade::MonteCarloSpec;
using nanotrade::OverfitReport;
using nanotrade::Random;
using nanotrade::StrategySpec;
using nanotrade::SweepAxis;
using nanotrade::SweepResult;
using nanotrade::SweepSpec;
using nanotrade::Trial;
using nanotrade::WalkForwardFold;
using nanotrade::WalkForwardResult;
using nanotrade::WalkForwardSpec;
using Cache = nanotrade::indicators::Cache;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

// ---- Comparisons ----

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// Bit for bit, except that any NaN matches any other.
bool same(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b) || (std::isnan(a) && std::isnan(b));
}

void expect_same(double actual, double expected, const std::string& message) {
    if (!same(actual, expected)) {
        throw check::Failure(message + " (expected " + text_of(expected) + ", got " + text_of(actual) + ")");
    }
}

void expect_same(const std::vector<double>& actual, const std::vector<double>& expected, const std::string& message) {
    check::expect_equal(actual.size(), expected.size(), message + ": size");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        expect_same(actual[i], expected[i], message + " [" + std::to_string(i) + "]");
    }
}

// `body` must throw a nanotrade::Error naming `field`.
template <typename Body>
void expect_error(Body&& body, const std::string& field, const std::string& message) {
    try {
        body();
    } catch (const nanotrade::Error& error) {
        check::expect_equal(error.field(), field, message + ": the field at fault (" + error.what() + ")");
        return;
    }
    throw check::Failure(message + " (did not throw)");
}

struct MetricField {
    const char* name;
    double Metrics::* member;
};

struct CountField {
    const char* name;
    std::size_t Metrics::* member;
};

constexpr std::array<MetricField, 29> metric_fields{{
    {"final_equity", &Metrics::final_equity},
    {"total_return", &Metrics::total_return},
    {"cagr", &Metrics::cagr},
    {"volatility", &Metrics::volatility},
    {"sharpe", &Metrics::sharpe},
    {"sortino", &Metrics::sortino},
    {"max_drawdown", &Metrics::max_drawdown},
    {"calmar", &Metrics::calmar},
    {"longest_drawdown_days", &Metrics::longest_drawdown_days},
    {"exposure", &Metrics::exposure},
    {"turnover", &Metrics::turnover},
    {"years", &Metrics::years},
    {"win_rate", &Metrics::win_rate},
    {"profit_factor", &Metrics::profit_factor},
    {"avg_trade", &Metrics::avg_trade},
    {"avg_win", &Metrics::avg_win},
    {"avg_loss", &Metrics::avg_loss},
    {"best_trade", &Metrics::best_trade},
    {"worst_trade", &Metrics::worst_trade},
    {"avg_bars_held", &Metrics::avg_bars_held},
    {"skewness", &Metrics::skewness},
    {"kurtosis", &Metrics::kurtosis},
    {"var_95", &Metrics::var_95},
    {"cvar_95", &Metrics::cvar_95},
    {"beta", &Metrics::beta},
    {"alpha", &Metrics::alpha},
    {"correlation", &Metrics::correlation},
    {"psr", &Metrics::psr},
    {"costs", &Metrics::costs},
}};

constexpr std::array<CountField, 3> count_fields{{
    {"longest_drawdown_bars", &Metrics::longest_drawdown_bars},
    {"trades", &Metrics::trades},
    {"max_losing_streak", &Metrics::max_losing_streak},
}};

void expect_same_metrics(const Metrics& actual, const Metrics& expected, const std::string& message) {
    for (const MetricField& metric : metric_fields) {
        expect_same(actual.*metric.member, expected.*metric.member, message + ": " + metric.name);
    }
    for (const CountField& count : count_fields) {
        check::expect_equal(actual.*count.member, expected.*count.member, message + ": " + count.name);
    }
}

void expect_same_overfit(const OverfitReport& actual, const OverfitReport& expected, const std::string& message) {
    check::expect_equal(actual.trials, expected.trials, message + ": overfitting trials");
    expect_same(actual.best_sharpe, expected.best_sharpe, message + ": best Sharpe ratio");
    expect_same(actual.expected_max_sharpe, expected.expected_max_sharpe, message + ": expected maximum Sharpe ratio");
    expect_same(actual.deflated_sharpe, expected.deflated_sharpe, message + ": deflated Sharpe ratio");
    check::expect_equal(actual.blocks, expected.blocks, message + ": blocks");
    check::expect_equal(actual.splits, expected.splits, message + ": splits");
    expect_same(actual.pbo, expected.pbo, message + ": PBO");
    expect_same(actual.oos_loss_probability, expected.oos_loss_probability, message + ": out-of-sample losses");
    expect_same(actual.logits, expected.logits, message + ": logits");
    check::expect_equal(actual.degradation.size(), expected.degradation.size(), message + ": degradation points");
    for (std::size_t i = 0; i < actual.degradation.size(); ++i) {
        for (std::size_t side = 0; side < 2; ++side) {
            expect_same(actual.degradation[i][side],
                        expected.degradation[i][side],
                        message + ": degradation point " + std::to_string(i));
        }
    }
}

void expect_same_sweeps(const SweepResult& actual, const SweepResult& expected, const std::string& message) {
    expect_same(actual.xs, expected.xs, message + ": xs");
    expect_same(actual.ys, expected.ys, message + ": ys");
    check::expect_equal(actual.trials.size(), expected.trials.size(), message + ": trials");
    for (std::size_t i = 0; i < actual.trials.size(); ++i) {
        const Trial& got = actual.trials[i];
        const Trial& want = expected.trials[i];
        const std::string where = message + ": trial " + std::to_string(i);
        expect_same(got.x, want.x, where + " x");
        expect_same(got.y, want.y, where + " y");
        check::expect_equal(got.valid, want.valid, where + " valid");
        check::expect_equal(got.error, want.error, where + " error");
        expect_same(got.score, want.score, where + " score");
        expect_same_metrics(got.metrics, want.metrics, where);
    }
    check::expect(actual.best == expected.best, message + ": the best trial");
    expect_same_overfit(actual.overfit, expected.overfit, message);
}

void expect_same_monte_carlo(const MonteCarloResult& actual,
                             const MonteCarloResult& expected,
                             const std::string& message) {
    check::expect_equal(actual.paths, expected.paths, message + ": paths");
    check::expect_equal(actual.block, expected.block, message + ": block");
    check::expect(actual.checkpoints == expected.checkpoints, message + ": checkpoints");
    for (std::size_t q = 0; q < actual.bands.size(); ++q) {
        expect_same(actual.bands[q], expected.bands[q], message + ": band " + std::to_string(q));
    }
    expect_same(actual.total_returns, expected.total_returns, message + ": total returns");
    expect_same(actual.cagrs, expected.cagrs, message + ": CAGRs");
    expect_same(actual.max_drawdowns, expected.max_drawdowns, message + ": maximum drawdowns");
    expect_same(actual.sharpes, expected.sharpes, message + ": Sharpe ratios");
    expect_same(actual.loss_probability, expected.loss_probability, message + ": loss probability");
}

// ---- Markets, strategies, and runs ----

// Takes a C string: GCC 13 warns of a dangling reference when a function returning one is passed a temporary string.
const BarSeries& stock(const char* symbol) {
    static std::map<std::string, BarSeries> loaded;
    auto found = loaded.find(symbol);
    if (found == loaded.end()) {
        const std::string path = std::string(NANOTRADE_DATA_DIR) + "/" + symbol + ".csv";
        found = loaded.emplace(symbol, nanotrade::load_csv_file(path, symbol).series).first;
    }
    return found->second;
}

BarSeries synthetic_market(std::uint64_t seed, std::size_t bars, double drift = 0.0, double volatility = 0.2) {
    nanotrade::SyntheticSpec spec;
    spec.seed = seed;
    spec.bars = bars;
    spec.drift = drift;
    spec.volatility = volatility;
    return nanotrade::synthesize(spec);
}

StrategySpec crossover() {
    StrategySpec spec;
    spec.id = "sma_cross";
    return spec;
}

StrategySpec crossover(double fast, double slow) {
    StrategySpec spec = crossover();
    spec.params["fast"] = fast;
    spec.params["slow"] = slow;
    return spec;
}

StrategySpec custom_rules(const std::string& long_entry,
                          const std::string& long_exit,
                          const std::string& param,
                          double value) {
    StrategySpec spec;
    spec.id = "custom";
    spec.rules.long_entry = long_entry;
    spec.rules.long_exit = long_exit;
    spec.params[param] = value;
    return spec;
}

SweepAxis make_axis(const std::string& param, double from, double to, double step) {
    SweepAxis out;
    out.param = param;
    out.from = from;
    out.to = to;
    out.step = step;
    return out;
}

// One backtest on its own, as a user would run it: fresh indicators, the signals, the simulation, and its metrics.
Metrics separate_run(const BarSeries& bars,
                     const StrategySpec& strategy,
                     const BacktestConfig& config,
                     std::size_t begin,
                     std::size_t end) {
    Cache fresh(bars);
    const nanotrade::Signals signals = nanotrade::generate_signals(strategy, fresh, config.allow_short);
    const BacktestResult result = nanotrade::run_backtest(bars, signals.direction, config, begin, end);
    return nanotrade::compute_metrics(bars, result, {}, config.risk_free_rate);
}

// An SMA crossover, long and short, for the bootstrap to resample.
BacktestResult crossover_backtest(const BarSeries& bars, std::size_t begin, std::size_t end) {
    Cache cache(bars);
    BacktestConfig config;
    config.allow_short = true;
    const nanotrade::Signals signals = nanotrade::generate_signals(crossover(10.0, 40.0), cache, config.allow_short);
    return nanotrade::run_backtest(bars, signals.direction, config, begin, end);
}

// Independent normal returns of 1% a bar, a row per trial.
std::vector<std::vector<double>> noise(std::uint64_t seed, std::size_t trials, std::size_t bars) {
    Random random(seed);
    std::vector<std::vector<double>> returns(trials, std::vector<double>(bars));
    for (std::vector<double>& row : returns) {
        for (double& r : row) {
            r = 0.01 * random.normal();
        }
    }
    return returns;
}

std::size_t choose(std::size_t n, std::size_t k) {
    std::size_t result = 1;
    for (std::size_t i = 1; i <= k; ++i) {
        result = result * (n - k + i) / i;
    }
    return result;
}

// The expected largest of n independent standard normal draws: the integral of x n phi(x) Phi(x)^(n - 1).
double expected_maximum_of_normals(std::size_t n) {
    constexpr double step = 1e-3;
    double total = 0.0;
    for (int i = -12'000; i <= 12'000; ++i) {
        const double x = static_cast<double>(i) * step;
        const double density = std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi);
        const double below = 0.5 * std::erfc(-x / std::numbers::sqrt2);
        total += x * static_cast<double>(n) * density * std::pow(below, static_cast<double>(n - 1)) * step;
    }
    return total;
}

// The annualized standard deviation of the log returns from close to close, at 252 trading days a year.
double realized_volatility(const BarSeries& bars) {
    std::vector<double> returns;
    for (std::size_t t = 1; t < bars.size(); ++t) {
        returns.push_back(std::log(bars.close[t] / bars.close[t - 1]));
    }
    double mean = 0.0;
    for (const double r : returns) {
        mean += r / static_cast<double>(returns.size());
    }
    double squares = 0.0;
    for (const double r : returns) {
        squares += (r - mean) * (r - mean);
    }
    return std::sqrt(squares / static_cast<double>(returns.size() - 1) * 252.0);
}

// The same bars up to `from`, and from there on prices bent by a slow wave, so every later bar is different.
BarSeries altered_from(const BarSeries& original, std::size_t from) {
    BarSeries out = original;
    for (std::size_t t = from; t < out.size(); ++t) {
        const double factor = 1.0 + 0.3 * std::sin(0.07 * static_cast<double>(t - from + 1));
        out.open[t] *= factor;
        out.high[t] *= factor;
        out.low[t] *= factor;
        out.close[t] *= factor;
    }
    return out;
}

BacktestConfig walk_forward_config() {
    BacktestConfig config;
    config.allow_short = true;
    config.commission_bps = 3.0;
    config.bars_per_year = 252.0;  // so two years are 504 bars and one is 252
    return config;
}

WalkForwardSpec walk_forward_spec(bool anchored) {
    WalkForwardSpec spec;
    spec.sweep.x = make_axis("fast", 5.0, 25.0, 10.0);
    spec.sweep.y = make_axis("slow", 40.0, 80.0, 40.0);
    spec.sweep.threads = 2;
    spec.in_sample_years = 2.0;
    spec.out_of_sample_years = 1.0;
    spec.anchored = anchored;
    return spec;
}

// ---- Sweeps ----

void sweep_cells_match_separate_backtests() {
    const BarSeries& bars = stock("AAPL");
    Cache cache(bars);
    BacktestConfig config;
    config.allow_short = true;
    config.commission_bps = 5.0;
    config.slippage_bps = 2.0;
    config.risk_free_rate = 0.02;
    constexpr std::size_t begin = 1500;
    constexpr std::size_t end = 4500;

    SweepSpec spec;
    spec.x = make_axis("fast", 5.0, 45.0, 20.0);
    spec.y = make_axis("slow", 30.0, 90.0, 30.0);
    spec.metric = "sortino";
    spec.threads = 3;
    const SweepResult grid = nanotrade::run_sweep(bars, cache, crossover(), config, begin, end, spec);
    expect_same(grid.xs, {5.0, 25.0, 45.0}, "the fast windows");
    expect_same(grid.ys, {30.0, 60.0, 90.0}, "the slow windows");
    check::expect_equal(grid.trials.size(), std::size_t{9}, "a trial per combination");
    for (std::size_t i = 0; i < grid.trials.size(); ++i) {
        const Trial& trial = grid.trials[i];
        const std::string where = "fast " + text_of(trial.x) + ", slow " + text_of(trial.y);
        expect_same(trial.x, grid.xs[i % 3], where + ": every x in a row");
        expect_same(trial.y, grid.ys[i / 3], where + ": a row per y");
        check::expect_equal(trial.valid, trial.x < trial.y, where + ": runs when the fast window is shorter");
        if (trial.valid) {
            const Metrics alone = separate_run(bars, crossover(trial.x, trial.y), config, begin, end);
            expect_same_metrics(trial.metrics, alone, where);
            expect_same(trial.score, alone.sortino, where + ": score");
        }
    }

    // One parameter: the other keeps the base strategy's value, and y is a single NaN.
    SweepSpec line;
    line.x = make_axis("slow", 40.0, 100.0, 30.0);
    line.threads = 2;
    StrategySpec base = crossover();
    base.params["fast"] = 10.0;
    const SweepResult row = nanotrade::run_sweep(bars, cache, base, config, begin, end, line);
    check::expect(row.ys.size() == 1 && std::isnan(row.ys[0]), "a one-parameter sweep has a single NaN y");
    check::expect_equal(row.trials.size(), std::size_t{3}, "a trial per slow window");
    for (const Trial& trial : row.trials) {
        const std::string where = "fast 10, slow " + text_of(trial.x);
        check::expect(trial.valid && std::isnan(trial.y), where + ": runs, with no y");
        const Metrics alone = separate_run(bars, crossover(10.0, trial.x), config, begin, end);
        expect_same_metrics(trial.metrics, alone, where);
        expect_same(trial.score, alone.sharpe, where + ": score");
    }
}

void each_metric_scores_the_trials() {
    const BarSeries& bars = stock("BA");
    Cache cache(bars);
    const BacktestConfig config{};
    struct Scoring {
        const char* metric;
        double (*score)(const Metrics&);
    };
    const std::array<Scoring, 8> scorings{{
        {"sharpe", [](const Metrics& m) { return m.sharpe; }},
        {"sortino", [](const Metrics& m) { return m.sortino; }},
        {"cagr", [](const Metrics& m) { return m.cagr; }},
        {"total_return", [](const Metrics& m) { return m.total_return; }},
        {"calmar", [](const Metrics& m) { return m.calmar; }},
        {"max_drawdown", [](const Metrics& m) { return -m.max_drawdown; }},
        {"profit_factor", [](const Metrics& m) { return m.profit_factor; }},
        {"win_rate", [](const Metrics& m) { return m.win_rate; }},
    }};
    StrategySpec base = crossover();
    base.params["slow"] = 60.0;
    for (const Scoring& scoring : scorings) {
        SweepSpec spec;
        spec.x = make_axis("fast", 10.0, 40.0, 15.0);
        spec.metric = scoring.metric;
        spec.threads = 1;
        spec.overfitting = false;
        const SweepResult result = nanotrade::run_sweep(bars, cache, base, config, 6000, 9000, spec);
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < result.trials.size(); ++i) {
            const Trial& trial = result.trials[i];
            const std::string where = std::string(scoring.metric) + ", fast " + text_of(trial.x);
            check::expect(trial.valid, where + ": runs");
            expect_same(trial.score, scoring.score(trial.metrics), where + ": score");
            if (!std::isnan(trial.score) && (!best || trial.score > result.trials[*best].score)) {
                best = i;
            }
        }
        check::expect(best.has_value() && result.best == best,
                      std::string(scoring.metric) + ": the best trial has the highest score");
    }

    SweepSpec unknown;
    unknown.x = make_axis("fast", 10.0, 40.0, 15.0);
    unknown.metric = "returns";
    expect_error([&] { static_cast<void>(nanotrade::run_sweep(bars, cache, base, config, 6000, 9000, unknown)); },
                 "sweep.metric",
                 "an unknown metric");
}

void one_and_eight_threads_agree() {
    const BarSeries& bars = stock("GE");
    BacktestConfig config;
    config.allow_short = true;
    config.commission_bps = 2.0;
    SweepSpec spec;
    spec.x = make_axis("fast", 5.0, 55.0, 10.0);
    spec.y = make_axis("slow", 20.0, 100.0, 20.0);  // 30 combinations, a few of them inverted
    spec.threads = 1;
    std::vector<double> reported;
    Cache cache_one(bars);
    const SweepResult one =
        nanotrade::run_sweep(bars, cache_one, crossover(), config, 3000, 6000, spec, [&](double done) {
            reported.push_back(done);
        });
    spec.threads = 8;
    Cache cache_eight(bars);
    const SweepResult eight = nanotrade::run_sweep(bars, cache_eight, crossover(), config, 3000, 6000, spec);
    const SweepResult warm = nanotrade::run_sweep(bars, cache_one, crossover(), config, 3000, 6000, spec);

    check::expect_equal(one.threads, std::size_t{1}, "one thread");
    check::expect_equal(eight.threads, std::size_t{8}, "eight threads");
    check::expect(one.overfit.splits > 0 && !std::isnan(one.overfit.pbo), "the overfitting estimate ran");
    expect_same_sweeps(eight, one, "eight threads against one");
    expect_same_sweeps(warm, one, "eight threads on a warm cache against one");

    check::expect(!reported.empty() && reported.back() == 1.0, "one thread reports progress up to 1");
    for (std::size_t i = 0; i < reported.size(); ++i) {
        check::expect(reported[i] > (i == 0 ? 0.0 : reported[i - 1]), "progress only grows");
    }
}

void invalid_combinations_fail_alone() {
    const BarSeries bars = synthetic_market(11, 1500);
    Cache cache(bars);
    SweepSpec spec;
    spec.x = make_axis("fast", 10.0, 50.0, 10.0);
    spec.y = make_axis("slow", 20.0, 60.0, 20.0);
    spec.threads = 4;
    const SweepResult result = nanotrade::run_sweep(bars, cache, crossover(), BacktestConfig{}, 0, bars.size(), spec);
    check::expect_equal(result.trials.size(), std::size_t{15}, "a trial per combination");
    std::size_t valid = 0;
    for (const Trial& trial : result.trials) {
        const std::string where = "fast " + text_of(trial.x) + ", slow " + text_of(trial.y);
        if (trial.x < trial.y) {
            check::expect(trial.valid && trial.error.empty(), where + ": runs");
            check::expect(trial.metrics.years > 0.0, where + ": has metrics");
            ++valid;
        } else {
            check::expect(!trial.valid, where + ": can't run");
            check::expect(trial.error.find("must be less than") != std::string::npos, where + ": says why");
        }
    }
    check::expect_equal(valid, std::size_t{9}, "the runnable combinations");
    check::expect(result.best.has_value() && result.trials[*result.best].valid, "the best trial is one that ran");
    check::expect_equal(result.overfit.trials, valid, "only the trials that ran count toward overfitting");

    // A custom rule's period must be a whole number of bars, so 0 and 7.5 fail and 15 and 30 run.
    SweepSpec periods;
    periods.x = make_axis("n", 0.0, 30.0, 7.5);
    const StrategySpec custom = custom_rules("close > sma(close, $n)", "", "n", 20.0);
    const SweepResult custom_result =
        nanotrade::run_sweep(bars, cache, custom, BacktestConfig{}, 0, bars.size(), periods);
    for (const Trial& trial : custom_result.trials) {
        const std::string where = "$n = " + text_of(trial.x);
        const bool whole = trial.x >= 1.0 && trial.x == std::floor(trial.x);
        check::expect_equal(trial.valid, whole, where + ": runs only as a whole number of bars");
        check::expect_equal(trial.error.empty(), whole, where + ": an error only when it can't run");
    }
}

void a_sweep_where_nothing_runs_throws() {
    const BarSeries bars = synthetic_market(12, 600);
    Cache cache(bars);
    SweepSpec inverted;
    inverted.x = make_axis("fast", 60.0, 80.0, 10.0);
    inverted.y = make_axis("slow", 20.0, 40.0, 10.0);
    expect_error(
        [&] { static_cast<void>(nanotrade::run_sweep(bars, cache, crossover(), {}, 0, bars.size(), inverted)); },
        "sweep",
        "every fast window above the slow one");

    SweepSpec periods;
    periods.x = make_axis("n", 10.0, 30.0, 10.0);
    const StrategySpec typo = custom_rules("close > sma(close, $n) and rsi(close, 2) < $oversold", "", "n", 20.0);
    expect_error([&] { static_cast<void>(nanotrade::run_sweep(bars, cache, typo, {}, 0, bars.size(), periods)); },
                 "sweep",
                 "a rule using a parameter with no value");
}

void sweep_axis_values() {
    const std::vector<double> tenths = make_axis("k", 0.0, 1.0, 0.1).values("sweep.x");
    check::expect_equal(tenths.size(), std::size_t{11}, "0 to 1 by 0.1");
    for (std::size_t i = 0; i < tenths.size(); ++i) {
        expect_same(tenths[i], static_cast<double>(i) / 10.0, "0 to 1 by 0.1, value " + std::to_string(i));
    }
    expect_same(tenths[3], 0.3, "three steps of 0.1 make exactly 0.3");
    expect_same(make_axis("k", 0.1, 0.3, 0.1).values("sweep.x"), {0.1, 0.2, 0.3}, "0.1 to 0.3 by 0.1");
    expect_same(make_axis("k", -0.3, 0.0, 0.1).values("sweep.x"), {-0.3, -0.2, -0.1, 0.0}, "-0.3 to 0 by 0.1");
    expect_same(make_axis("fast", 5.0, 5.0, 1.0).values("sweep.x"), {5.0}, "a range of one value");
    expect_same(make_axis("fast", 5.0, 9.0, 10.0).values("sweep.x"), {5.0}, "a step longer than the range");
    expect_same(make_axis("fast", 5.0, 20.0, 4.0).values("sweep.x"), {5.0, 9.0, 13.0, 17.0}, "a ragged step");
    check::expect_equal(make_axis("n", 1.0, 10'000.0, 1.0).values("sweep.x").size(),
                        std::size_t{10'000},
                        "10,000 values are allowed");

    expect_error([] { static_cast<void>(make_axis("n", 1.0, 10'001.0, 1.0).values("sweep.x")); },
                 "sweep.x.step",
                 "10,001 values");
    expect_error([] { static_cast<void>(make_axis("k", 0.0, 1.0, 1e-4).values("sweep.y")); },
                 "sweep.y.step",
                 "10,001 values by 0.0001");
    expect_error([] { static_cast<void>(make_axis("fast", 10.0, 9.5, 1.0).values("sweep.x")); },
                 "sweep.x.to",
                 "an empty range");
    expect_error([] { static_cast<void>(make_axis("fast", 50.0, 5.0, 5.0).values("sweep.y")); },
                 "sweep.y.to",
                 "a reversed range");
    for (const double step : {0.0, -0.0, -1.0, nan, inf}) {
        expect_error([&] { static_cast<void>(make_axis("fast", 5.0, 50.0, step).values("sweep.x")); },
                     "sweep.x.step",
                     "a step of " + text_of(step));
    }
    expect_error([] { static_cast<void>(make_axis("", 5.0, 50.0, 5.0).values("sweep.x")); },
                 "sweep.x.param",
                 "no parameter");
}

void sweep_axis_blames_the_bound_at_fault() {
    expect_error([] { static_cast<void>(make_axis("fast", nan, 50.0, 5.0).values("sweep.x")); },
                 "sweep.x.from",
                 "a range from NaN");
    expect_error([] { static_cast<void>(make_axis("fast", 5.0, inf, 5.0).values("sweep.x")); },
                 "sweep.x.to",
                 "a range to infinity");
}

void sweeping_a_missing_parameter_is_rejected() {
    const BarSeries bars = synthetic_market(13, 400);
    Cache cache(bars);
    const BacktestConfig config{};
    const auto sweep = [&](const StrategySpec& strategy, const SweepSpec& spec, std::size_t from, std::size_t to) {
        return nanotrade::run_sweep(bars, cache, strategy, config, from, to, spec);
    };
    const auto rejects =
        [&](const StrategySpec& strategy, const SweepSpec& spec, const std::string& field, const std::string& message) {
            expect_error([&] { static_cast<void>(sweep(strategy, spec, 0, bars.size())); }, field, message);
        };

    SweepSpec spec;
    spec.x = make_axis("period", 5.0, 20.0, 5.0);
    rejects(crossover(), spec, "sweep.x.param", "the SMA crossover has no period");
    spec.x = make_axis("fast", 5.0, 20.0, 5.0);
    spec.y = make_axis("signal", 5.0, 9.0, 2.0);
    rejects(crossover(), spec, "sweep.y.param", "nor a signal window");
    spec.y = make_axis("fast", 5.0, 20.0, 5.0);
    rejects(crossover(), spec, "sweep.y.param", "the same parameter on both axes");
    StrategySpec unknown = crossover();
    unknown.id = "sma_crossing";
    spec.y.reset();
    rejects(unknown, spec, "strategy.id", "an unknown strategy");

    const StrategySpec custom = custom_rules("close > sma(close, $n)", "", "n", 20.0);
    SweepSpec unused;
    unused.x = make_axis("m", 5.0, 20.0, 5.0);
    rejects(custom, unused, "sweep.x.param", "custom rules that don't use $m");
    unused.x = make_axis("n", 5.0, 20.0, 5.0);
    unused.y = make_axis("m", 5.0, 20.0, 5.0);
    rejects(custom, unused, "sweep.y.param", "nor on the y axis");

    // A parameter only the exit rule uses is swept.
    const StrategySpec exit_only = custom_rules("close > sma(close, 50)", "close < sma(close, $n)", "n", 10.0);
    SweepSpec used;
    used.x = make_axis("n", 10.0, 30.0, 10.0);
    const SweepResult ran = sweep(exit_only, used, 0, bars.size());
    check::expect(ran.trials.size() == 3 &&
                      std::all_of(ran.trials.begin(), ran.trials.end(), [](const Trial& trial) { return trial.valid; }),
                  "a parameter only the exit rule uses");

    SweepSpec huge;
    huge.x = make_axis("fast", 2.0, 102.0, 1.0);
    huge.y = make_axis("slow", 105.0, 204.0, 1.0);
    rejects(crossover(), huge, "sweep", "101 by 100 combinations");
    expect_error([&] { static_cast<void>(sweep(crossover(), spec, 10, 10)); }, "range", "an empty date range");
    expect_error([&] { static_cast<void>(sweep(crossover(), spec, 0, bars.size() + 1)); },
                 "range",
                 "a range past the last bar");
}

// A plain backtest blames a typo in a rule on that rule; a sweep of a parameter the rule uses should too.
void a_rule_typo_in_a_sweep_names_the_rule() {
    const BarSeries bars = synthetic_market(14, 400);
    Cache cache(bars);
    const StrategySpec typo = custom_rules("close = sma(close, $n)", "", "n", 20.0);
    expect_error([&] { static_cast<void>(nanotrade::generate_signals(typo, cache, false)); },
                 "strategy.long_entry",
                 "a backtest of the rule");
    SweepSpec spec;
    spec.x = make_axis("n", 10.0, 30.0, 10.0);
    expect_error([&] { static_cast<void>(nanotrade::run_sweep(bars, cache, typo, {}, 0, bars.size(), spec)); },
                 "strategy.long_entry",
                 "a sweep of the rule");
}

// ---- Overfitting ----

// The PBO of a single noise matrix scatters widely around 0.5 (by 0.18 over 200 seeds), so the band is checked on the
// mean over eight of them.
void pbo_of_noise_is_near_one_half() {
    constexpr std::uint64_t seeds = 8;
    double total = 0.0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        const OverfitReport report = nanotrade::probability_of_overfitting(noise(seed, 40, 2000));
        const std::string where = "noise from seed " + std::to_string(seed);
        check::expect_equal(report.trials, std::size_t{40}, where + ": trials");
        check::expect_equal(report.blocks, std::size_t{16}, where + ": blocks");
        check::expect(report.pbo > 0.0 && report.pbo < 1.0, where + ": PBO " + text_of(report.pbo));
        check::expect(report.oos_loss_probability >= 0.0 && report.oos_loss_probability <= 1.0,
                      where + ": out-of-sample loss probability " + text_of(report.oos_loss_probability));
        total += report.pbo;
    }
    const double mean = total / static_cast<double>(seeds);
    check::expect(mean >= 0.3 && mean <= 0.7, "the mean PBO of pure noise: " + text_of(mean));
}

void pbo_of_a_strong_edge_is_small() {
    std::vector<std::vector<double>> returns = noise(4, 40, 2000);
    for (double& r : returns[17]) {
        r += 0.002;  // a Sharpe ratio of 0.2 a bar, over 3 a year
    }
    const OverfitReport report = nanotrade::probability_of_overfitting(returns);
    check::expect(report.pbo < 0.1, "PBO " + text_of(report.pbo));
    check::expect(report.oos_loss_probability < 0.1,
                  "the winner loses out of sample with probability " + text_of(report.oos_loss_probability));
}

void cscv_split_count() {
    struct Case {
        std::size_t bars;
        std::size_t max_blocks;
        std::size_t blocks;
    };
    const std::array<Case, 7> cases{{
        {2000, 16, 16},
        {2000, 64, 16},
        {2000, 8, 8},
        {2000, 7, 6},
        {2000, 4, 4},
        {40, 16, 8},  // blocks of at least five bars
        {25, 16, 4},
    }};
    check::expect_equal(choose(16, 8), std::size_t{12'870}, "C(16, 8)");
    for (const Case& c : cases) {
        const OverfitReport report = nanotrade::probability_of_overfitting(noise(9, 5, c.bars), c.max_blocks);
        const std::string where = std::to_string(c.bars) + " bars, at most " + std::to_string(c.max_blocks) + " blocks";
        check::expect_equal(report.blocks, c.blocks, where + ": blocks");
        check::expect_equal(report.splits, choose(report.blocks, report.blocks / 2), where + ": splits");
        check::expect_equal(report.logits.size(), std::min<std::size_t>(report.splits, 1000), where + ": logits kept");
        check::expect_equal(report.degradation.size(), report.logits.size(), where + ": degradation points kept");
        check::expect(report.pbo >= 0.0 && report.pbo <= 1.0, where + ": PBO " + text_of(report.pbo));
    }
}

void pbo_needs_two_trials() {
    const OverfitReport none = nanotrade::probability_of_overfitting({});
    check::expect(std::isnan(none.pbo) && std::isnan(none.oos_loss_probability), "no trials: NaN");
    check::expect_equal(none.splits, std::size_t{0}, "no trials: no splits");
    const OverfitReport one = nanotrade::probability_of_overfitting(noise(10, 1, 2000));
    check::expect(std::isnan(one.pbo), "one trial: NaN, not " + text_of(one.pbo));
    check::expect(std::isnan(one.oos_loss_probability), "one trial: no out-of-sample loss probability");
    check::expect_equal(one.splits, std::size_t{0}, "one trial: no splits");
    const OverfitReport two = nanotrade::probability_of_overfitting(noise(11, 2, 2000));
    check::expect(two.pbo >= 0.0 && two.pbo <= 1.0, "two trials are enough: " + text_of(two.pbo));
    check::expect(std::isnan(nanotrade::probability_of_overfitting(noise(12, 10, 19)).pbo),
                  "19 bars can't make four blocks of five");
    check::expect(std::isnan(nanotrade::probability_of_overfitting(noise(13, 10, 2000), 3).pbo),
                  "fewer than four blocks");
    std::vector<std::vector<double>> ragged = noise(14, 3, 200);
    ragged[1].pop_back();
    check::expect_throws<nanotrade::Error>([&] { static_cast<void>(nanotrade::probability_of_overfitting(ragged)); },
                                           "trials of different lengths");
}

void expected_max_sharpe_grows_with_trials() {
    check::expect_equal(nanotrade::expected_max_sharpe(1.0, 1), 0.0, "one trial");
    check::expect_equal(nanotrade::expected_max_sharpe(1.0, 0), 0.0, "no trials");
    check::expect_equal(nanotrade::expected_max_sharpe(0.0, 100), 0.0, "trials that all agree");
    double previous = 0.0;
    const std::array<std::size_t, 9> counts{2, 3, 5, 10, 30, 100, 1000, 10'000, 1'000'000};
    for (const std::size_t trials : counts) {
        const double value = nanotrade::expected_max_sharpe(1.0, trials);
        check::expect(value > previous,
                      std::to_string(trials) + " trials: " + text_of(value) + " after " + text_of(previous));
        previous = value;
    }
    check::expect_near(nanotrade::expected_max_sharpe(4.0, 50),
                       2.0 * nanotrade::expected_max_sharpe(1.0, 50),
                       1e-12,
                       "it scales with the standard deviation of the Sharpe ratios");
    for (const std::size_t trials : {std::size_t{10}, std::size_t{100}, std::size_t{1000}}) {
        check::expect_near(nanotrade::expected_max_sharpe(1.0, trials),
                           expected_maximum_of_normals(trials),
                           0.05,
                           "the largest of " + std::to_string(trials) + " standard normals");
    }
}

void deflated_sharpe_is_at_most_psr() {
    const BarSeries& bars = stock("CAT");
    Cache cache(bars);
    const BacktestConfig config{};
    const double annualize = std::sqrt(bars.bars_per_year());
    const std::size_t begin = 5000;
    const std::size_t end = bars.size();
    for (const char* metric : {"sharpe", "cagr"}) {
        SweepSpec spec;
        spec.x = make_axis("fast", 10.0, 50.0, 10.0);
        spec.y = make_axis("slow", 60.0, 210.0, 50.0);
        spec.metric = metric;
        spec.threads = 4;
        const SweepResult result = nanotrade::run_sweep(bars, cache, crossover(), config, begin, end, spec);
        const std::string where = std::string("the best by ") + metric;
        check::expect(result.best.has_value(), where + ": exists");
        const Metrics& best = result.trials[*result.best].metrics;
        const OverfitReport& overfit = result.overfit;
        expect_same(overfit.best_sharpe, best.sharpe, where + ": its Sharpe ratio");
        check::expect(overfit.expected_max_sharpe > 0.0, where + ": luck alone gives some Sharpe ratio");
        check::expect(overfit.deflated_sharpe >= 0.0 && overfit.deflated_sharpe <= best.psr,
                      where + ": deflated " + text_of(overfit.deflated_sharpe) + ", plain " + text_of(best.psr));
        const double deflated = nanotrade::probabilistic_sharpe(best.sharpe / annualize,
                                                                overfit.expected_max_sharpe / annualize,
                                                                end - begin,
                                                                best.skewness,
                                                                best.kurtosis);
        check::expect_near(overfit.deflated_sharpe, deflated, 1e-9, where + ": the PSR against the expected maximum");
    }
}

// ---- Walk-forward ----

void walk_forward_folds_tile_the_range() {
    const BarSeries bars = synthetic_market(21, 2520, 0.05, 0.25);
    Cache cache(bars);
    constexpr std::size_t begin = 100;
    constexpr std::size_t end = 2500;
    constexpr std::size_t in_sample = 504;
    constexpr std::size_t out_of_sample = 252;
    for (const bool anchored : {false, true}) {
        const std::string mode = anchored ? "anchored" : "rolling";
        const WalkForwardResult result = nanotrade::run_walk_forward(bars,
                                                                     cache,
                                                                     crossover(),
                                                                     walk_forward_config(),
                                                                     begin,
                                                                     end,
                                                                     walk_forward_spec(anchored));
        const std::vector<WalkForwardFold>& folds = result.folds;
        check::expect_equal(folds.size(),
                            (end - begin - in_sample + out_of_sample - 1) / out_of_sample,
                            mode + ": folds");
        for (std::size_t f = 0; f < folds.size(); ++f) {
            const WalkForwardFold& fold = folds[f];
            const std::string where = mode + " fold " + std::to_string(f);
            check::expect_equal(fold.is_begin,
                                anchored ? begin : begin + f * out_of_sample,
                                where + ": in-sample start");
            check::expect_equal(fold.is_end, begin + in_sample + f * out_of_sample, where + ": in-sample end");
            check::expect_equal(fold.oos_begin, fold.is_end, where + ": out of sample starts where in sample ends");
            check::expect_equal(fold.oos_end,
                                std::min(fold.oos_begin + out_of_sample, end),
                                where + ": out-of-sample end");
            check::expect(fold.oos_begin < fold.oos_end, where + ": out of sample isn't empty");
            if (f > 0) {
                check::expect_equal(fold.oos_begin,
                                    folds[f - 1].oos_end,
                                    where + ": out of sample starts where the last one ended");
            }
            check::expect(fold.x < fold.y, where + ": a combination that runs");
            check::expect_equal(fold.oos_trades, fold.out_of_sample.trades, where + ": trades");
        }
        check::expect_equal(folds.front().is_begin, begin, mode + ": the first fold starts the range");
        check::expect_equal(folds.back().oos_end, end, mode + ": the last fold ends it");
        check::expect_equal(result.stitched.begin, folds.front().oos_begin, mode + ": stitched start");
        check::expect_equal(result.stitched.end, end, mode + ": stitched end");
        check::expect_equal(result.stitched.equity.size(), end - folds.front().oos_begin, mode + ": stitched bars");
        check::expect_equal(result.benchmark.equity.size(), result.stitched.equity.size(), mode + ": benchmark bars");
    }
}

void walk_forward_stitches_equity() {
    const BarSeries bars = synthetic_market(22, 2520, 0.05, 0.25);
    Cache cache(bars);
    const BacktestConfig config = walk_forward_config();
    const WalkForwardSpec spec = walk_forward_spec(false);
    const WalkForwardResult result = nanotrade::run_walk_forward(bars, cache, crossover(), config, 100, 2500, spec);
    SweepSpec in_sample_sweep = spec.sweep;
    in_sample_sweep.overfitting = false;
    double capital = config.initial_capital;
    std::size_t offset = 0;
    for (std::size_t f = 0; f < result.folds.size(); ++f) {
        const WalkForwardFold& fold = result.folds[f];
        const std::string where = "fold " + std::to_string(f);
        // The fold trades the in-sample winner...
        const SweepResult optimized =
            nanotrade::run_sweep(bars, cache, crossover(), config, fold.is_begin, fold.is_end, in_sample_sweep);
        check::expect(optimized.best.has_value(), where + ": an in-sample winner");
        const Trial& winner = optimized.trials[*optimized.best];
        expect_same(fold.x, winner.x, where + ": fast window");
        expect_same(fold.y, winner.y, where + ": slow window");
        expect_same_metrics(fold.in_sample, winner.metrics, where + " in sample");
        // ...out of sample, from the equity the fold before it ended with.
        BacktestConfig fold_config = config;
        fold_config.initial_capital = capital;
        fold_config.close_at_end = true;
        const nanotrade::Signals signals =
            nanotrade::generate_signals(crossover(fold.x, fold.y), cache, config.allow_short);
        const BacktestResult run =
            nanotrade::run_backtest(bars, signals.direction, fold_config, fold.oos_begin, fold.oos_end);
        check::expect(offset + run.equity.size() <= result.stitched.equity.size(), where + ": in the stitched run");
        for (std::size_t i = 0; i < run.equity.size(); ++i) {
            expect_same(result.stitched.equity[offset + i],
                        run.equity[i],
                        where + ": stitched equity at bar " + std::to_string(fold.oos_begin + i));
        }
        expect_same_metrics(fold.out_of_sample,
                            nanotrade::compute_metrics(bars, run, {}, config.risk_free_rate),
                            where + " out of sample");
        offset += run.equity.size();
        capital = run.equity.back();
    }
    check::expect_equal(offset, result.stitched.equity.size(), "the folds make up the stitched equity");
    expect_same(result.stitched.initial_capital, config.initial_capital, "the stitched run starts with the capital");
    expect_same(result.stitched_metrics.final_equity, capital, "and ends with the last fold's equity");
}

void walk_forward_does_not_peek() {
    const BarSeries bars = synthetic_market(23, 2520, 0.05, 0.25);
    Cache cache(bars);
    const BacktestConfig config = walk_forward_config();
    const WalkForwardSpec spec = walk_forward_spec(false);
    const WalkForwardResult original = nanotrade::run_walk_forward(bars, cache, crossover(), config, 100, 2500, spec);
    const std::size_t last = original.folds.size() - 1;
    for (const std::size_t k : std::array<std::size_t, 3>{0, last / 2, last}) {
        const std::size_t cut = original.folds[k].is_end;
        const BarSeries altered = altered_from(bars, cut);
        Cache altered_cache(altered);
        const WalkForwardResult changed =
            nanotrade::run_walk_forward(altered, altered_cache, crossover(), config, 100, 2500, spec);
        check::expect_equal(changed.folds.size(), original.folds.size(), "the same folds");
        for (std::size_t j = 0; j <= k; ++j) {
            const WalkForwardFold& got = changed.folds[j];
            const WalkForwardFold& want = original.folds[j];
            const std::string where =
                "fold " + std::to_string(j) + " with every bar from " + std::to_string(cut) + " altered";
            expect_same(got.x, want.x, where + ": fast window");
            expect_same(got.y, want.y, where + ": slow window");
            expect_same_metrics(got.in_sample, want.in_sample, where + ": in sample");
            if (j < k) {
                expect_same_metrics(got.out_of_sample, want.out_of_sample, where + ": out of sample");
            }
        }
        check::expect(!same(changed.folds[k].out_of_sample.final_equity, original.folds[k].out_of_sample.final_equity),
                      "altering the bars from " + std::to_string(cut) + " changes the out-of-sample result there");
    }
}

void walk_forward_rejects_bad_windows() {
    const BarSeries bars = synthetic_market(24, 5000);
    Cache cache(bars);
    const BacktestConfig config = walk_forward_config();
    const auto walk = [&](std::size_t from, std::size_t to, double in_years, double out_years) {
        WalkForwardSpec spec = walk_forward_spec(false);
        spec.in_sample_years = in_years;
        spec.out_of_sample_years = out_years;
        return nanotrade::run_walk_forward(bars, cache, crossover(), config, from, to, spec);
    };
    expect_error([&] { static_cast<void>(walk(0, 600, 2.0, 1.0)); }, "walkforward", "504 + 252 bars in 600");
    check::expect_equal(walk(96, 600, 1.0, 1.0).folds.size(), std::size_t{1}, "252 + 252 bars in 504: one fold");
    expect_error([&] { static_cast<void>(walk(97, 600, 1.0, 1.0)); }, "walkforward", "252 + 252 bars in 503");
    expect_error([&] { static_cast<void>(walk(0, bars.size() + 1, 2.0, 1.0)); },
                 "walkforward",
                 "a range past the last bar");
    for (const double years : {0.2, 51.0, nan}) {
        expect_error([&] { static_cast<void>(walk(0, bars.size(), years, 1.0)); },
                     "walkforward.in_sample_years",
                     "an in-sample window of " + text_of(years) + " years");
    }
    for (const double years : {0.04, 21.0, nan}) {
        expect_error([&] { static_cast<void>(walk(0, bars.size(), 2.0, years)); },
                     "walkforward.out_of_sample_years",
                     "an out-of-sample window of " + text_of(years) + " years");
    }
    expect_error([&] { static_cast<void>(walk(0, bars.size(), 0.25, 0.05)); },
                 "walkforward.out_of_sample_years",
                 "hundreds of folds");
}

// ---- Monte Carlo ----

void monte_carlo_is_deterministic_per_seed() {
    const BarSeries bars = synthetic_market(31, 1300, 0.03);
    const BacktestResult backtest = crossover_backtest(bars, 50, 1300);
    const double years = nanotrade::compute_metrics(bars, backtest, {}, 0.0).years;
    MonteCarloSpec spec;
    spec.paths = 300;
    spec.block = 15;
    spec.seed = 42;
    const MonteCarloResult first = nanotrade::run_monte_carlo(backtest, years, spec);
    const MonteCarloResult again = nanotrade::run_monte_carlo(backtest, years, spec);
    expect_same_monte_carlo(again, first, "the same seed");

    spec.seed = 43;
    const MonteCarloResult other = nanotrade::run_monte_carlo(backtest, years, spec);
    check::expect(other.checkpoints == first.checkpoints, "the checkpoints depend only on the backtest");
    check::expect_equal(other.total_returns.size(), first.total_returns.size(), "another seed, as many paths");
    std::size_t differences = 0;
    for (std::size_t i = 0; i < first.total_returns.size(); ++i) {
        differences += same(other.total_returns[i], first.total_returns[i]) ? 0 : 1;
    }
    check::expect(differences > first.total_returns.size() / 2,
                  "another seed, other paths: " + std::to_string(differences) + " total returns differ");
    check::expect(!same(other.bands[2].back(), first.bands[2].back()), "another seed, another median");
}

void monte_carlo_bands_are_ordered() {
    const BarSeries bars = synthetic_market(32, 1300, 0.03);
    const BacktestResult backtest = crossover_backtest(bars, 50, 1300);
    const double years = nanotrade::compute_metrics(bars, backtest, {}, 0.0).years;
    const std::size_t count = backtest.equity.size();
    for (const std::size_t block : std::array<std::size_t, 3>{1, 20, 400}) {
        MonteCarloSpec spec;
        spec.paths = 400;
        spec.block = block;
        spec.seed = 9;
        const MonteCarloResult result = nanotrade::run_monte_carlo(backtest, years, spec);
        const std::string where = "blocks of " + std::to_string(block);
        check::expect_equal(result.paths, spec.paths, where + ": paths");
        check::expect_equal(result.block, block, where + ": block");

        const std::vector<std::size_t>& points = result.checkpoints;
        check::expect(!points.empty() && points.size() <= 160, where + ": checkpoints");
        check::expect_equal(points.front(), std::size_t{0}, where + ": the first checkpoint");
        check::expect_equal(points.back(), count - 1, where + ": the last checkpoint");
        for (std::size_t k = 1; k < points.size(); ++k) {
            check::expect(points[k - 1] < points[k], where + ": checkpoints in order");
        }
        for (const std::vector<double>& band : result.bands) {
            check::expect_equal(band.size(), points.size(), where + ": a band value per checkpoint");
        }
        for (std::size_t k = 0; k < points.size(); ++k) {
            for (std::size_t q = 1; q < result.bands.size(); ++q) {
                check::expect(result.bands[q - 1][k] <= result.bands[q][k],
                              where + ": band " + std::to_string(q - 1) + " at most band " + std::to_string(q) +
                                  " at checkpoint " + std::to_string(k));
            }
        }

        const std::array<std::pair<const char*, const std::vector<double>*>, 4> distributions{{
            {"total returns", &result.total_returns},
            {"CAGRs", &result.cagrs},
            {"maximum drawdowns", &result.max_drawdowns},
            {"Sharpe ratios", &result.sharpes},
        }};
        for (const auto& [name, values] : distributions) {
            check::expect(!values->empty() && values->size() <= spec.paths, where + ": " + name + " per path");
            check::expect(std::is_sorted(values->begin(), values->end()), where + ": " + name + " sorted");
        }
        check::expect_equal(result.total_returns.size(), spec.paths, where + ": a total return per path");
        check::expect(result.max_drawdowns.front() >= 0.0 && result.max_drawdowns.back() <= 1.0,
                      where + ": drawdowns are fractions");

        const double losses = static_cast<double>(
            std::count_if(result.total_returns.begin(), result.total_returns.end(), [](double r) { return r < 0.0; }));
        check::expect(result.loss_probability >= 0.0 && result.loss_probability <= 1.0,
                      where + ": loss probability " + text_of(result.loss_probability));
        expect_same(result.loss_probability,
                    losses / static_cast<double>(spec.paths),
                    where + ": the share of paths that lose");

        const std::vector<double>& totals = result.total_returns;
        const double middle = 0.5 * static_cast<double>(totals.size() - 1);
        const auto below = static_cast<std::size_t>(middle);
        const double median =
            totals[below] + (totals[below + 1] - totals[below]) * (middle - static_cast<double>(below));
        check::expect_near(result.bands[2].back(),
                           backtest.initial_capital * (1.0 + median),
                           1e-9 * backtest.initial_capital,
                           where + ": the median band ends at the median path");
    }
}

void monte_carlo_rejects_bad_specs() {
    const BarSeries bars = synthetic_market(33, 200);
    const BacktestResult backtest = crossover_backtest(bars, 140, 200);
    const auto resample = [&](const BacktestResult& result, std::size_t paths, std::size_t block) {
        MonteCarloSpec spec;
        spec.paths = paths;
        spec.block = block;
        return nanotrade::run_monte_carlo(result, 60.0 / 252.0, spec);
    };
    for (const std::size_t paths : std::array<std::size_t, 3>{0, 9, 10'001}) {
        expect_error([&] { static_cast<void>(resample(backtest, paths, 20)); },
                     "montecarlo.paths",
                     std::to_string(paths) + " paths");
    }
    for (const std::size_t block : std::array<std::size_t, 2>{0, 1001}) {
        expect_error([&] { static_cast<void>(resample(backtest, 100, block)); },
                     "montecarlo.block",
                     "blocks of " + std::to_string(block));
    }
    check::expect_equal(resample(backtest, 10, 1).total_returns.size(), std::size_t{10}, "10 paths of single bars");
    check::expect_equal(resample(backtest, 10'000, 1000).total_returns.size(),
                        std::size_t{10'000},
                        "10,000 paths of blocks of 1,000 bars");
    const BacktestResult one_bar = crossover_backtest(bars, 199, 200);
    expect_error([&] { static_cast<void>(resample(one_bar, 100, 20)); }, "range", "a backtest of one bar");
}

// ---- Synthetic markets ----

void synthetic_is_deterministic() {
    nanotrade::SyntheticSpec spec;
    spec.seed = 5;
    spec.bars = 1000;
    spec.drift = 0.1;
    spec.volatility = 0.3;
    const BarSeries first = nanotrade::synthesize(spec);
    const BarSeries again = nanotrade::synthesize(spec);
    check::expect_equal(again.symbol, first.symbol, "symbol");
    check::expect(again.time == first.time, "the same timestamps");
    expect_same(again.open, first.open, "open");
    expect_same(again.high, first.high, "high");
    expect_same(again.low, first.low, "low");
    expect_same(again.close, first.close, "close");
    expect_same(again.volume, first.volume, "volume");

    spec.seed = 6;
    const BarSeries other = nanotrade::synthesize(spec);
    check::expect(other.time == first.time, "another seed, the same calendar");
    std::size_t differences = 0;
    for (std::size_t t = 0; t < first.size(); ++t) {
        differences += same(other.close[t], first.close[t]) ? 0 : 1;
    }
    check::expect(differences > 990, "another seed, another market: " + std::to_string(differences) + " closes differ");
}

void synthetic_bars_are_consistent() {
    const std::int64_t first_day = nanotrade::days_from_civil(2000, 1, 3);
    for (const double volatility : {0.05, 0.2, 1.5}) {
        const BarSeries bars = synthetic_market(17, 3000, 0.0, volatility);
        const std::string where = "volatility " + text_of(volatility);
        check::expect_equal(bars.size(), std::size_t{3000}, where + ": bars");
        check::expect_equal(bars.time.front(), first_day * nanotrade::seconds_per_day, where + ": the first bar");
        check::expect_equal(nanotrade::format_date(bars.time.front()),
                            std::string("2000-01-03"),
                            where + ": the first date");
        for (std::size_t t = 0; t < bars.size(); ++t) {
            const std::string at = where + ", bar " + std::to_string(t);
            const nanotrade::Timestamp time = bars.time[t];
            check::expect(time % nanotrade::seconds_per_day == 0, at + ": at midnight");
            const std::int64_t day = time / nanotrade::seconds_per_day;
            const std::int64_t weekday = (day + 4) % 7;  // 0 is Sunday: 1970-01-01 was a Thursday
            check::expect(weekday >= 1 && weekday <= 5, at + ": " + nanotrade::format_date(time) + " is a weekday");
            if (t > 0) {
                const std::int64_t gap = day - bars.time[t - 1] / nanotrade::seconds_per_day;
                check::expect(gap == (weekday == 1 ? 3 : 1), at + ": the next business day");
            }
            const double open = bars.open[t];
            const double high = bars.high[t];
            const double low = bars.low[t];
            const double close = bars.close[t];
            check::expect(std::isfinite(high) && low > 0.0, at + ": positive, finite prices");
            check::expect(low <= std::min(open, close) && high >= std::max(open, close),
                          at + ": the high and low contain the open and close");
            check::expect(std::isfinite(bars.volume[t]) && bars.volume[t] >= 0.0, at + ": volume");
        }
    }
}

void synthetic_volatility_matches() {
    for (const double volatility : {0.1, 0.2, 0.45}) {
        for (const std::uint64_t seed : std::array<std::uint64_t, 3>{1, 2, 3}) {
            const double realized = realized_volatility(synthetic_market(seed, 5000, 0.05, volatility));
            check::expect(realized >= 0.7 * volatility && realized <= 1.3 * volatility,
                          "seed " + std::to_string(seed) + ": asked for " + text_of(volatility) + ", realized " +
                              text_of(realized));
        }
    }
}

void synthetic_specs_are_validated() {
    struct Case {
        const char* what;
        std::size_t bars;
        double drift;
        double volatility;
        double start_price;
        const char* field;
    };
    const std::array<Case, 15> rejected{{
        {"no bars", 0, 0.0, 0.2, 100.0, "data.synthetic.bars"},
        {"49 bars", 49, 0.0, 0.2, 100.0, "data.synthetic.bars"},
        {"20,001 bars", 20'001, 0.0, 0.2, 100.0, "data.synthetic.bars"},
        {"a drift above 100%", 252, 1.01, 0.2, 100.0, "data.synthetic.drift"},
        {"a drift below -100%", 252, -1.01, 0.2, 100.0, "data.synthetic.drift"},
        {"a NaN drift", 252, nan, 0.2, 100.0, "data.synthetic.drift"},
        {"no volatility", 252, 0.0, 0.0, 100.0, "data.synthetic.volatility"},
        {"negative volatility", 252, 0.0, -0.2, 100.0, "data.synthetic.volatility"},
        {"volatility above 200%", 252, 0.0, 2.01, 100.0, "data.synthetic.volatility"},
        {"NaN volatility", 252, 0.0, nan, 100.0, "data.synthetic.volatility"},
        {"infinite volatility", 252, 0.0, inf, 100.0, "data.synthetic.volatility"},
        {"a start price of 0", 252, 0.0, 0.2, 0.0, "data.synthetic.start_price"},
        {"a negative start price", 252, 0.0, 0.2, -5.0, "data.synthetic.start_price"},
        {"a start price above a million", 252, 0.0, 0.2, 1.5e6, "data.synthetic.start_price"},
        {"a NaN start price", 252, 0.0, 0.2, nan, "data.synthetic.start_price"},
    }};
    const std::array<Case, 7> accepted{{
        {"50 bars", 50, 0.0, 0.2, 100.0, ""},
        {"20,000 bars", 20'000, 0.0, 0.2, 100.0, ""},
        {"a drift of 100%", 252, 1.0, 0.2, 100.0, ""},
        {"a drift of -100%", 252, -1.0, 0.2, 100.0, ""},
        {"volatility of 200%", 252, 0.0, 2.0, 100.0, ""},
        {"a start price of a million", 252, 0.0, 0.2, 1e6, ""},
        {"a start price of a cent", 252, 0.0, 0.2, 0.01, ""},
    }};
    const auto spec_of = [](const Case& c) {
        nanotrade::SyntheticSpec spec;
        spec.bars = c.bars;
        spec.drift = c.drift;
        spec.volatility = c.volatility;
        spec.start_price = c.start_price;
        return spec;
    };
    for (const Case& c : rejected) {
        expect_error([&] { static_cast<void>(nanotrade::synthesize(spec_of(c))); }, c.field, c.what);
    }
    for (const Case& c : accepted) {
        check::expect_equal(nanotrade::synthesize(spec_of(c)).size(), c.bars, c.what);
    }
}

}  // namespace

int main() {
    return check::run({
        {"sweep cells match separate backtests exactly", sweep_cells_match_separate_backtests},
        {"each metric scores the trials, higher being better", each_metric_scores_the_trials},
        {"one thread and eight give bit-identical sweeps", one_and_eight_threads_agree},
        {"invalid combinations fail alone, with a reason", invalid_combinations_fail_alone},
        {"a sweep where nothing can run throws", a_sweep_where_nothing_runs_throws},
        {"sweep axis values, and ranges and steps rejected", sweep_axis_values},
        {"a non-finite bound is blamed on that bound", sweep_axis_blames_the_bound_at_fault},
        {"sweeping a parameter the strategy doesn't use is rejected", sweeping_a_missing_parameter_is_rejected},
        {"a typo in a swept rule is blamed on the rule", a_rule_typo_in_a_sweep_names_the_rule},
        {"the PBO of pure noise is near one half", pbo_of_noise_is_near_one_half},
        {"the PBO of a strong edge among noise is small", pbo_of_a_strong_edge_is_small},
        {"CSCV makes C(blocks, blocks / 2) splits", cscv_split_count},
        {"the PBO needs two trials", pbo_needs_two_trials},
        {"the expected maximum Sharpe ratio grows with the trials", expected_max_sharpe_grows_with_trials},
        {"the deflated Sharpe ratio is at most the plain PSR", deflated_sharpe_is_at_most_psr},
        {"walk-forward folds tile the range", walk_forward_folds_tile_the_range},
        {"walk-forward equity is stitched continuously", walk_forward_stitches_equity},
        {"walk-forward chooses without peeking", walk_forward_does_not_peek},
        {"walk-forward windows too long for the range are rejected", walk_forward_rejects_bad_windows},
        {"monte carlo is deterministic per seed", monte_carlo_is_deterministic_per_seed},
        {"monte carlo bands are ordered and distributions sorted", monte_carlo_bands_are_ordered},
        {"monte carlo rejects bad paths and blocks", monte_carlo_rejects_bad_specs},
        {"synthetic markets are deterministic per seed", synthetic_is_deterministic},
        {"synthetic bars are consistent weekdays from 2000-01-03", synthetic_bars_are_consistent},
        {"synthetic volatility is the one asked for", synthetic_volatility_matches},
        {"synthetic specs are validated", synthetic_specs_are_validated},
    });
}
