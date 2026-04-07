#include "nanotrade/backtest_engine.h"

#include <algorithm>
#include <stdexcept>

namespace nanotrade {
namespace {

struct PendingOrder {
    bool active = false;
    Side side = Side::Buy;
    std::string reason;
};

double applySlippage(double reference_price, Side side, double slippage_bps) {
    const double slippage_fraction = slippage_bps / 10000.0;
    return side == Side::Buy
        ? reference_price * (1.0 + slippage_fraction)
        : reference_price * (1.0 - slippage_fraction);
}

double markToMarketEquity(double cash, int position_quantity, double close_price) {
    return cash + (static_cast<double>(position_quantity) * close_price);
}

void executeOrder(
    const Bar& bar,
    bool fill_at_close,
    Side side,
    const std::string& reason,
    const BacktestConfig& config,
    double& cash,
    int& position_quantity,
    std::vector<Trade>& trades
) {
    const double reference_price = fill_at_close ? bar.close : bar.open;
    const double fill_price = applySlippage(reference_price, side, config.slippage_bps);
    const double notional = fill_price * static_cast<double>(config.order_quantity);
    const double commission = config.commission_per_trade;

    if (side == Side::Buy) {
        const double total_cost = notional + commission;
        if (cash + 1e-12 < total_cost) {
            return;
        }

        cash -= total_cost;
        position_quantity += config.order_quantity;
    } else {
        if (position_quantity < config.order_quantity) {
            throw std::logic_error("Attempted to sell more shares than the portfolio holds.");
        }

        cash += (notional - commission);
        position_quantity -= config.order_quantity;
    }

    const double equity_after = markToMarketEquity(cash, position_quantity, bar.close);
    trades.push_back(Trade{bar.date, side, config.order_quantity, fill_price, commission, cash, equity_after, reason});
}

}  // namespace

std::string toString(Side side) {
    switch (side) {
        case Side::Buy:
            return "buy";
        case Side::Sell:
            return "sell";
    }

    return "unknown";
}

BacktestResult BacktestEngine::run(
    const std::vector<Bar>& bars,
    const Strategy& strategy,
    const BacktestConfig& config
) {
    return run(bars, strategy.generateSignals(bars), strategy.name(), config);
}

BacktestResult BacktestEngine::run(
    const std::vector<Bar>& bars,
    const std::vector<Signal>& signals,
    const std::string& strategy_name,
    const BacktestConfig& config
) {
    MarketDataLoader::validateBars(bars);
    validateConfig(config);

    if (signals.size() != bars.size()) {
        throw std::invalid_argument("Signal count must match the number of bars.");
    }
    if (strategy_name.empty()) {
        throw std::invalid_argument("Strategy name must not be empty.");
    }
    if (config.fill_model != FillModel::NextBarOpen) {
        throw std::invalid_argument("Unsupported fill model.");
    }

    double cash = config.initial_cash;
    int position_quantity = 0;
    PendingOrder pending_order;
    std::vector<Trade> trades;
    std::vector<EquityPoint> equity_curve;
    equity_curve.reserve(bars.size());

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const Bar& bar = bars[i];

        if (pending_order.active) {
            executeOrder(
                bar,
                false,
                pending_order.side,
                pending_order.reason,
                config,
                cash,
                position_quantity,
                trades
            );
            pending_order = {};
        }

        if (i + 1 < bars.size()) {
            if (signals[i] == Signal::Long && position_quantity == 0) {
                pending_order = PendingOrder{true, Side::Buy, strategy_name + "_enter"};
            } else if (signals[i] == Signal::Flat && position_quantity > 0) {
                pending_order = PendingOrder{true, Side::Sell, strategy_name + "_exit"};
            }
        }

        const double market_value = static_cast<double>(position_quantity) * bar.close;
        const double equity = cash + market_value;
        equity_curve.push_back(EquityPoint{bar.date, cash, position_quantity, market_value, equity});
    }

    if (position_quantity > 0 && config.close_open_position_on_last_bar) {
        executeOrder(
            bars.back(),
            true,
            Side::Sell,
            "end_of_backtest",
            config,
            cash,
            position_quantity,
            trades
        );

        EquityPoint& final_point = equity_curve.back();
        final_point.cash = cash;
        final_point.position_quantity = position_quantity;
        final_point.market_value = 0.0;
        final_point.equity = cash;
    }

    BacktestResult result;
    result.strategy_name = strategy_name;
    result.config = config;
    result.signals = signals;
    result.trades = std::move(trades);
    result.equity_curve = std::move(equity_curve);
    result.metrics.final_equity = result.equity_curve.back().equity;
    result.metrics.net_profit = result.metrics.final_equity - config.initial_cash;
    result.metrics.total_return = result.metrics.net_profit / config.initial_cash;
    result.metrics.max_drawdown = computeMaxDrawdown(result.equity_curve);
    result.metrics.bars_processed = bars.size();
    result.metrics.trades_executed = result.trades.size();
    return result;
}

void BacktestEngine::validateConfig(const BacktestConfig& config) {
    if (config.initial_cash <= 0.0) {
        throw std::invalid_argument("Initial cash must be greater than zero.");
    }
    if (config.order_quantity <= 0) {
        throw std::invalid_argument("Order quantity must be greater than zero.");
    }
    if (config.commission_per_trade < 0.0) {
        throw std::invalid_argument("Commission must not be negative.");
    }
    if (config.slippage_bps < 0.0) {
        throw std::invalid_argument("Slippage must not be negative.");
    }
}

double BacktestEngine::computeMaxDrawdown(const std::vector<EquityPoint>& equity_curve) {
    if (equity_curve.empty()) {
        return 0.0;
    }

    double peak = equity_curve.front().equity;
    double max_drawdown = 0.0;

    for (const EquityPoint& point : equity_curve) {
        peak = std::max(peak, point.equity);
        const double drawdown = peak == 0.0 ? 0.0 : (peak - point.equity) / peak;
        max_drawdown = std::max(max_drawdown, drawdown);
    }

    return max_drawdown;
}

}  // namespace nanotrade
