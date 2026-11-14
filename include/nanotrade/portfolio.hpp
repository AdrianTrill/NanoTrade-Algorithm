#ifndef NANOTRADE_PORTFOLIO_HPP
#define NANOTRADE_PORTFOLIO_HPP

#include "nanotrade/engine.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/metrics.hpp"
#include "nanotrade/research.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/strategy.hpp"
#include "nanotrade/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A portfolio runs one strategy on several markets at once. Each market trades in a sleeve of the capital exactly as a
// backtest on that market alone would: the sleeve's value follows that backtest's equity, scaled to the capital the
// sleeve holds. On a schedule the sleeves are set back to their target weights at a close, paying the configured
// commission and slippage on what that buys and sells in each market. Only the dates every market traded count.
namespace nanotrade {

enum class Weighting : std::uint8_t {
    Equal,              // the same weight for each market
    Fixed,              // the weights given
    InverseVolatility,  // each market's weight in inverse proportion to its recent volatility
};

enum class Rebalance : std::uint8_t {
    Never,
    Monthly,  // at the last close of each month
    Quarterly,
    Yearly,
};

struct PortfolioSpec {
    Weighting weighting{Weighting::Equal};
    std::vector<double> weights;  // Fixed: one per market, none negative and not all zero; scaled to sum to one
    Rebalance rebalance{Rebalance::Monthly};
    std::size_t volatility_window{63};  // InverseVolatility: daily returns behind each weight, about three months
};

inline constexpr std::size_t max_portfolio_markets = 20;

struct PortfolioMarket {
    const BarSeries* bars{nullptr};
    indicators::Cache* cache{nullptr};  // the indicators of those bars, which the strategy reads
};

struct PortfolioSleeve {
    std::size_t begin{0};  // the market's own bars traded, [begin, end)
    std::size_t end{0};
    BacktestReport report;       // the strategy on this market alone over those bars, with buy and hold beside it
    double target{0.0};          // its first target weight
    double final_weight{0.0};    // its share of the portfolio at the last close
    double contribution{0.0};    // what its own trading gained or lost, as a fraction of the initial capital
    std::vector<double> weight;  // its share of the portfolio at each close
};

struct PortfolioResult {
    BarSeries calendar;        // the dates every market traded, and no prices: the time axis of `result`
    BacktestResult result;     // over calendar bars [result.begin, result.end): the sleeves' trades pooled, the costs
    Metrics metrics;           // with `benchmark` as the benchmark for beta, alpha, and correlation
    BacktestResult benchmark;  // the same markets bought and held, weighted and rebalanced the same way
    Metrics benchmark_metrics;
    std::vector<PortfolioSleeve> sleeves;
    std::vector<std::vector<double>> correlation;  // of the sleeves' daily returns; NaN for a sleeve that never moved
    std::size_t rebalances{0};
    double rebalance_traded{0.0};  // value bought and sold in the markets to rebalance
    double rebalance_cost{0.0};    // the commissions and slippage on it, also counted in `result`
    std::size_t fills{0};          // the sleeves' fills and the rebalancing trades
};

// The portfolio over the dates from `from` to `to`, both included when given. Throws nanotrade::Error naming the field
// at fault: portfolio.markets, portfolio.weights, range, or one of the strategy's or config's.
PortfolioResult run_portfolio(std::span<const PortfolioMarket> markets,
                              const StrategySpec& strategy,
                              const BacktestConfig& config,
                              const PortfolioSpec& spec,
                              std::optional<Timestamp> from,
                              std::optional<Timestamp> to);

}  // namespace nanotrade

#endif
