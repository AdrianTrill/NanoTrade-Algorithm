#ifndef NANOTRADE_REPORTING_H
#define NANOTRADE_REPORTING_H

#include <cstddef>
#include <string>

#include "nanotrade/backtest_engine.h"

namespace nanotrade {

std::string formatSummary(const BacktestResult& result, std::size_t max_trades_to_print = 10);

}  // namespace nanotrade

#endif
