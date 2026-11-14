#ifndef NANOTRADE_ENGINE_HPP
#define NANOTRADE_ENGINE_HPP

#include "nanotrade/series.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nanotrade {

enum class Sizing : std::uint8_t {
    Percent,     // a fixed fraction of equity per position
    Volatility,  // scaled so the position's volatility matches a target
};

struct BacktestConfig {
    double initial_capital{100'000.0};
    bool allow_short{false};
    Sizing sizing{Sizing::Percent};
    double position_size{1.0};          // percent sizing: the fraction of equity a position takes
    double target_volatility{0.15};     // volatility sizing: annualized
    std::size_t volatility_window{20};  // bars of returns the realized volatility is measured over
    double max_leverage{1.0};           // largest position value as a multiple of equity
    double rebalance_threshold{0.25};   // volatility sizing: resize when the target drifts this far from the position
    double commission_bps{0.0};         // per fill, in basis points of its value
    double commission_fixed{0.0};       // per fill
    double slippage_bps{0.0};           // each fill's price moved against it
    double borrow_rate{0.0};            // annual cost of a short position, as a fraction of its value
    double stop_loss{0.0};              // exit when the price moves this fraction against the entry; 0 is off
    double take_profit{0.0};            // exit when it moves this fraction in favor; 0 is off
    double trailing_stop{0.0};          // exit this fraction below the highest high since entry (above the low
                                        // for a short); 0 is off
    bool fractional_shares{false};
    bool close_at_end{true};     // close a position still open at the last bar's close
    double risk_free_rate{0.0};  // annual, for the Sharpe and Sortino ratios
    double bars_per_year{0.0};   // 0: measured from the timestamps
};

// Throws nanotrade::Error naming the config field at fault.
void validate(const BacktestConfig& config);

struct Fill {
    std::size_t bar{0};
    double shares{0.0};  // bought if positive, sold if negative
    double price{0.0};   // after slippage
    double commission{0.0};
    double slippage{0.0};  // what the slippage cost
    std::string reason;    // entry, exit, reverse, rebalance, stop_loss, take_profit, trailing_stop, end
};

struct Trade {
    int direction{1};  // 1 long, -1 short
    std::size_t entry_bar{0};
    std::size_t exit_bar{0};
    double entry_price{0.0};  // average over the fills that built the position
    double exit_price{0.0};   // average over the fills that reduced it
    double shares{0.0};       // the largest size held
    double pnl{0.0};          // after commissions, slippage, and borrowing
    double return_pct{0.0};   // pnl over the entry value at the largest size
    double mae{0.0};          // maximum adverse excursion, as a fraction of the entry price
    double mfe{0.0};          // maximum favorable excursion
    std::string exit_reason;
    bool open{false};  // still open when the backtest ended
};

struct BacktestResult {
    std::size_t begin{0};  // the bars traded, [begin, end)
    std::size_t end{0};
    double initial_capital{0.0};
    double bars_per_year{252.0};
    std::vector<double> equity;    // at each bar's close
    std::vector<double> position;  // shares held at each close, negative when short
    std::vector<double> exposure;  // position value over equity at each close
    std::vector<Fill> fills;
    std::vector<Trade> trades;
    double commissions{0.0};
    double slippage{0.0};
    double borrow{0.0};
    double traded_value{0.0};
    std::size_t trimmed{0};  // orders cut down to fit the leverage cap after the price gapped
    bool ruined{false};      // equity reached zero and trading stopped
};

// Runs `direction` (one value per bar, read at the close) over bars [begin, end). An order decided at a bar's close
// fills at the next bar's open, so a signal can never trade on the prices it was computed from. Bars before `begin`
// only warm up the indicators; an order decided at bar begin - 1 fills at the open of bar begin.
BacktestResult run_backtest(const BarSeries& bars,
                            std::span<const std::int8_t> direction,
                            const BacktestConfig& config,
                            std::size_t begin,
                            std::size_t end);

}  // namespace nanotrade

#endif
