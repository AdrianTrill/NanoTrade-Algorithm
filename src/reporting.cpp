#include "nanotrade/reporting.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace nanotrade {
namespace {

std::string formatDouble(double value) {
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(2) << value;
    return formatted.str();
}

void appendRow(std::ostringstream& output, const std::string& label, const std::string& value) {
    output << "  " << std::left << std::setw(18) << label << value << '\n';
}

}  // namespace

std::string formatSummary(const BacktestResult& result, std::size_t max_trades_to_print) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2);

    const std::string start_date = result.equity_curve.empty() ? "n/a" : result.equity_curve.front().date;
    const std::string end_date = result.equity_curve.empty() ? "n/a" : result.equity_curve.back().date;

    output << "Backtest Summary\n";
    appendRow(output, "Strategy", result.strategy_name);
    appendRow(output, "Date range", start_date + " -> " + end_date);
    appendRow(output, "Bars processed", std::to_string(result.metrics.bars_processed));
    appendRow(output, "Initial cash", formatDouble(result.config.initial_cash));
    appendRow(output, "Final equity", formatDouble(result.metrics.final_equity));
    appendRow(output, "Net profit", formatDouble(result.metrics.net_profit));
    appendRow(output, "Total return", formatDouble(result.metrics.total_return * 100.0) + "%");
    appendRow(output, "Max drawdown", formatDouble(result.metrics.max_drawdown * 100.0) + "%");
    appendRow(output, "Executed trades", std::to_string(result.metrics.trades_executed));

    output << '\n' << "Execution Model\n";
    appendRow(output, "Signal timing", "bar close");
    appendRow(output, "Fill timing", "next bar open");
    appendRow(output, "Position model", "long-only, fixed quantity");
    appendRow(output, "Order quantity", std::to_string(result.config.order_quantity));
    appendRow(output, "Commission", formatDouble(result.config.commission_per_trade));
    appendRow(output, "Slippage", formatDouble(result.config.slippage_bps) + " bps");

    if (result.trades.empty()) {
        output << "\nTrade Log\n";
        output << "  none\n";
        return output.str();
    }

    output << "\nTrade Log\n";
    output << "  " << std::left
           << std::setw(12) << "Date"
           << std::setw(8) << "Side"
           << std::setw(8) << "Qty"
           << std::setw(12) << "Price"
           << std::setw(12) << "Comm"
           << std::setw(14) << "Cash After"
           << std::setw(14) << "Equity After"
           << "Reason\n";

    const std::size_t trade_count_to_print = std::min(max_trades_to_print, result.trades.size());

    for (std::size_t i = 0; i < trade_count_to_print; ++i) {
        const Trade& trade = result.trades[i];
        output << "  " << std::left
               << std::setw(12) << trade.date
               << std::setw(8) << toString(trade.side)
               << std::setw(8) << trade.quantity
               << std::setw(12) << trade.price
               << std::setw(12) << trade.commission
               << std::setw(14) << trade.cash_after
               << std::setw(14) << trade.equity_after
               << trade.reason << '\n';
    }

    if (trade_count_to_print < result.trades.size()) {
        output << "  ... " << (result.trades.size() - trade_count_to_print) << " more trades not shown\n";
    }

    return output.str();
}

}  // namespace nanotrade
