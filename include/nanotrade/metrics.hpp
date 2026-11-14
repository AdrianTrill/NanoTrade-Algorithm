#ifndef NANOTRADE_METRICS_HPP
#define NANOTRADE_METRICS_HPP

#include "nanotrade/engine.hpp"
#include "nanotrade/series.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace nanotrade {

// Ratios that can't be computed, such as a Sharpe ratio with no volatility, are NaN, which JSON writes as null.
struct Metrics {
    double final_equity{0.0};
    double total_return{0.0};
    double cagr{0.0};
    double volatility{0.0};  // annualized
    double sharpe{0.0};
    double sortino{0.0};
    double max_drawdown{0.0};  // as a positive fraction
    double calmar{0.0};
    std::size_t longest_drawdown_bars{0};
    double longest_drawdown_days{0.0};
    double exposure{0.0};  // fraction of bars holding a position
    double turnover{0.0};  // traded value over average equity, per year
    double years{0.0};
    std::size_t trades{0};
    double win_rate{0.0};
    double profit_factor{0.0};
    double avg_trade{0.0};  // mean trade return
    double avg_win{0.0};
    double avg_loss{0.0};
    double best_trade{0.0};
    double worst_trade{0.0};
    double avg_bars_held{0.0};
    std::size_t max_losing_streak{0};
    double skewness{0.0};
    double kurtosis{0.0};  // excess
    double var_95{0.0};    // one-bar historical value at risk, as a positive fraction
    double cvar_95{0.0};
    double beta{0.0};   // against the benchmark
    double alpha{0.0};  // annualized
    double correlation{0.0};
    double psr{0.0};    // probability that the true Sharpe ratio is above zero
    double costs{0.0};  // commissions, slippage, and borrowing
};

// Each bar's return, the first relative to the initial capital.
std::vector<double> bar_returns(const BacktestResult& result);

Metrics compute_metrics(const BarSeries& bars,
                        const BacktestResult& result,
                        std::span<const double> benchmark_returns,
                        double risk_free_rate);

struct ReturnMoments {
    std::size_t count{0};
    double mean{0.0};
    double stdev{0.0};     // sample
    double skewness{0.0};  // population
    double kurtosis{0.0};  // population, excess
};

ReturnMoments moments(std::span<const double> returns);

// Probabilistic Sharpe ratio (Bailey and Lopez de Prado, 2012): the probability that the true per-bar Sharpe ratio
// exceeds `benchmark`, given a per-bar estimate `sharpe` from `count` returns with that skewness and excess kurtosis.
double probabilistic_sharpe(double sharpe, double benchmark, std::size_t count, double skewness, double kurtosis);

struct DrawdownPeriod {
    std::size_t peak{0};  // bar index, relative to the start of the window
    std::size_t trough{0};
    std::size_t recovery{0};  // equals the window's length when not recovered
    bool recovered{false};
    double depth{0.0};
    bool from_initial{false};  // fell from the initial capital, before any close of the window reached it
};

std::vector<DrawdownPeriod> worst_drawdowns(std::span<const double> equity, double initial, std::size_t count);
std::vector<double> drawdown_series(std::span<const double> equity, double initial);

// The bars a drawdown spans, as indices into the series: from its peak, which is the close before the window when it
// fell from the initial capital, as CAGR counts, to its recovery, or to the window's last bar.
struct DrawdownSpan {
    std::size_t first{0};
    std::size_t last{0};
};
DrawdownSpan drawdown_span(const DrawdownPeriod& period, std::size_t begin, std::size_t end);

struct YearReturns {
    std::int64_t year{0};
    std::array<double, 12> months{};  // NaN for months outside the window
    double total{0.0};
};

std::vector<YearReturns> calendar_returns(const BarSeries& bars, const BacktestResult& result);

}  // namespace nanotrade

#endif
