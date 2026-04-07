#ifndef NANOTRADE_BACKTEST_ENGINE_H
#define NANOTRADE_BACKTEST_ENGINE_H

#include <cstddef>
#include <string>
#include <vector>

#include "nanotrade/market_data.h"
#include "nanotrade/strategies.h"

namespace nanotrade {

enum class Side {
    Buy,
    Sell
};

std::string toString(Side side);

enum class FillModel {
    NextBarOpen
};

struct BacktestConfig {
    double initial_cash = 10000.0;
    int order_quantity = 10;
    double commission_per_trade = 0.0;
    double slippage_bps = 0.0;
    FillModel fill_model = FillModel::NextBarOpen;
    bool close_open_position_on_last_bar = true;
};

struct Trade {
    std::string date;
    Side side = Side::Buy;
    int quantity = 0;
    double price = 0.0;
    double commission = 0.0;
    double cash_after = 0.0;
    double equity_after = 0.0;
    std::string reason;
};

struct EquityPoint {
    std::string date;
    double cash = 0.0;
    int position_quantity = 0;
    double market_value = 0.0;
    double equity = 0.0;
};

struct BacktestMetrics {
    double final_equity = 0.0;
    double net_profit = 0.0;
    double total_return = 0.0;
    double max_drawdown = 0.0;
    std::size_t bars_processed = 0;
    std::size_t trades_executed = 0;
};

struct BacktestResult {
    std::string strategy_name;
    BacktestConfig config;
    std::vector<Signal> signals;
    std::vector<Trade> trades;
    std::vector<EquityPoint> equity_curve;
    BacktestMetrics metrics;
};

class BacktestEngine {
public:
    static BacktestResult run(
        const std::vector<Bar>& bars,
        const Strategy& strategy,
        const BacktestConfig& config = {}
    );

    static BacktestResult run(
        const std::vector<Bar>& bars,
        const std::vector<Signal>& signals,
        const std::string& strategy_name,
        const BacktestConfig& config = {}
    );

private:
    static void validateConfig(const BacktestConfig& config);
    static double computeMaxDrawdown(const std::vector<EquityPoint>& equity_curve);
};

}  // namespace nanotrade

#endif
