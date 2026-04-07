#include "nanotrade/reporting.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace nanotrade {

std::string formatSummary(const BacktestResult& result, std::size_t max_trades_to_print) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2);

    output << "Strategy: " << result.strategy_name << '\n';
    output << "Bars processed: " << result.metrics.bars_processed << '\n';
    output << "Initial cash: " << result.config.initial_cash << '\n';
    output << "Final equity: " << result.metrics.final_equity << '\n';
    output << "Net profit: " << result.metrics.net_profit << '\n';
    output << "Total return: " << (result.metrics.total_return * 100.0) << "%\n";
    output << "Max drawdown: " << (result.metrics.max_drawdown * 100.0) << "%\n";
    output << "Executed trades: " << result.metrics.trades_executed << '\n';
    output << "Execution model: signals evaluated on the close, orders filled on the next bar open, long-only, "
              "fixed quantity, optional forced close on the final bar\n";
    output << "Trade assumptions: quantity=" << result.config.order_quantity
           << ", commission=" << result.config.commission_per_trade
           << ", slippage=" << result.config.slippage_bps << " bps\n";

    if (result.trades.empty()) {
        output << "\nTrades: none\n";
        return output.str();
    }

    output << "\nTrades:\n";
    const std::size_t trade_count_to_print = std::min(max_trades_to_print, result.trades.size());

    for (std::size_t i = 0; i < trade_count_to_print; ++i) {
        const Trade& trade = result.trades[i];
        output << "  " << trade.date
               << "  " << toString(trade.side)
               << "  qty=" << trade.quantity
               << "  price=" << trade.price
               << "  commission=" << trade.commission
               << "  cash_after=" << trade.cash_after
               << "  reason=" << trade.reason << '\n';
    }

    if (trade_count_to_print < result.trades.size()) {
        output << "  ... " << (result.trades.size() - trade_count_to_print) << " more trades not shown\n";
    }

    return output.str();
}

}  // namespace nanotrade
