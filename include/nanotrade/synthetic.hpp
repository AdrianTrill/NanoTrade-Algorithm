#ifndef NANOTRADE_SYNTHETIC_HPP
#define NANOTRADE_SYNTHETIC_HPP

#include "nanotrade/series.hpp"

#include <cstddef>
#include <cstdint>

namespace nanotrade {

// A random market: daily bars on business days from 2000-01-03, log returns with a drift, GARCH(1,1) volatility
// clustering around a long-run level, and occasional jumps. It has no structure a strategy could exploit, so whatever
// a backtest finds on it is luck, which makes it the control for an overfitting experiment. A seed gives the same
// market on every platform.
struct SyntheticSpec {
    std::uint64_t seed{7};
    std::size_t bars{2520};
    double drift{0.0};       // expected annual return
    double volatility{0.2};  // long-run annual volatility
    double start_price{100.0};
};

BarSeries synthesize(const SyntheticSpec& spec);

}  // namespace nanotrade

#endif
