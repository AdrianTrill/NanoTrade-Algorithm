#ifndef NANOTRADE_RESEARCH_HPP
#define NANOTRADE_RESEARCH_HPP

#include "nanotrade/engine.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/metrics.hpp"
#include "nanotrade/strategy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nanotrade {

// Called now and then with the fraction of the work done.
using Progress = std::function<void(double)>;

// One backtest of a strategy, with buy and hold over the same bars as its benchmark.
struct BacktestReport {
    Signals signals;
    BacktestResult result;
    Metrics metrics;
    BacktestResult benchmark;
    Metrics benchmark_metrics;
};

BacktestReport run_report(const BarSeries& bars,
                          indicators::Cache& cache,
                          const StrategySpec& strategy,
                          const BacktestConfig& config,
                          std::size_t begin,
                          std::size_t end);

// The buy-and-hold benchmark: the whole equity in the asset from the first bar, paying the same costs.
BacktestResult run_benchmark(const BarSeries& bars, const BacktestConfig& config, std::size_t begin, std::size_t end);

struct SweepAxis {
    std::string param;
    double from{0.0};
    double to{0.0};
    double step{1.0};

    std::vector<double> values(const std::string& field) const;  // throws on an empty or oversized range
};

struct SweepSpec {
    SweepAxis x;
    std::optional<SweepAxis> y;
    std::string metric{"sharpe"};  // sharpe, sortino, cagr, total_return, calmar, max_drawdown, profit_factor
    std::size_t threads{0};        // 0: one per core, and one in WebAssembly
    bool overfitting{true};        // also estimate the deflated Sharpe ratio and the probability of overfitting
};

inline constexpr std::size_t max_sweep_trials = 10'000;

struct Trial {
    double x{0.0};
    double y{0.0};
    bool valid{false};
    std::string error;  // why the combination can't run, such as an inverted pair of windows
    double score{0.0};  // the chosen metric, higher is better; NaN when it can't be computed
    Metrics metrics;
};

struct OverfitReport {
    std::size_t trials{0};
    // The deflated Sharpe ratio (Bailey and Lopez de Prado, 2014) of the selected trial.
    double best_sharpe{0.0};          // annualized
    double expected_max_sharpe{0.0};  // annualized: the best Sharpe ratio luck alone would give among the trials
    double deflated_sharpe{0.0};      // probability the selected trial's true Sharpe ratio beats that
    // The probability of backtest overfitting, by combinatorially symmetric cross-validation (Bailey, Borwein,
    // Lopez de Prado, and Zhu, 2017).
    std::size_t blocks{0};
    std::size_t splits{0};
    double pbo{0.0};
    double oos_loss_probability{0.0};  // splits where the in-sample winner lost money out of sample
    // For an even sample of at most 1,000 splits: the logit of the winner's out-of-sample rank, and its in- and
    // out-of-sample Sharpe ratios (per bar).
    std::vector<double> logits;
    std::vector<std::array<double, 2>> degradation;
};

struct SweepResult {
    std::vector<double> xs;
    std::vector<double> ys;     // a single NaN for a one-parameter sweep
    std::vector<Trial> trials;  // row-major: for each y, every x
    std::optional<std::size_t> best;
    OverfitReport overfit;
    std::size_t threads{1};
};

SweepResult run_sweep(const BarSeries& bars,
                      indicators::Cache& cache,
                      const StrategySpec& base,
                      const BacktestConfig& config,
                      std::size_t begin,
                      std::size_t end,
                      const SweepSpec& spec,
                      const Progress& progress = {});

// CSCV on a matrix of per-bar returns, one row per trial, kept as block sums. Exposed for tests.
OverfitReport probability_of_overfitting(const std::vector<std::vector<double>>& trial_returns,
                                         std::size_t max_blocks = 16);
double expected_max_sharpe(double sharpe_variance, std::size_t trials);

struct WalkForwardSpec {
    SweepSpec sweep;
    double in_sample_years{4.0};
    double out_of_sample_years{1.0};
    bool anchored{false};
};

struct WalkForwardFold {
    std::size_t is_begin{0};
    std::size_t is_end{0};
    std::size_t oos_begin{0};
    std::size_t oos_end{0};
    double x{0.0};
    double y{0.0};
    Metrics in_sample;
    Metrics out_of_sample;
    std::size_t oos_trades{0};
};

struct WalkForwardResult {
    std::vector<WalkForwardFold> folds;
    BacktestResult stitched;  // the out-of-sample windows chained, each starting from the last one's equity
    Metrics stitched_metrics;
    BacktestResult benchmark;
    Metrics benchmark_metrics;
    double efficiency{0.0};  // mean out-of-sample CAGR over mean in-sample CAGR
};

WalkForwardResult run_walk_forward(const BarSeries& bars,
                                   indicators::Cache& cache,
                                   const StrategySpec& base,
                                   const BacktestConfig& config,
                                   std::size_t begin,
                                   std::size_t end,
                                   const WalkForwardSpec& spec,
                                   const Progress& progress = {});

struct MonteCarloSpec {
    std::size_t paths{1000};
    std::size_t block{20};  // mean block length of the stationary bootstrap, in bars
    std::uint64_t seed{1};
};

struct MonteCarloResult {
    std::size_t paths{0};
    std::size_t block{0};
    std::vector<std::size_t> checkpoints;      // bar offsets where the bands are measured
    std::array<std::vector<double>, 5> bands;  // equity at the 5th, 25th, 50th, 75th, and 95th percentiles
    std::vector<double> total_returns;         // per path, sorted
    std::vector<double> cagrs;                 // per path, sorted
    std::vector<double> max_drawdowns;         // per path, sorted
    std::vector<double> sharpes;               // per path, sorted
    double loss_probability{0.0};              // paths that end below the initial capital
};

// Resamples a backtest's per-bar returns with the stationary bootstrap (Politis and Romano, 1994): blocks of random
// length, so the volatility clustering within them survives.
MonteCarloResult run_monte_carlo(const BacktestResult& result,
                                 double years,
                                 const MonteCarloSpec& spec,
                                 const Progress& progress = {});

}  // namespace nanotrade

#endif
