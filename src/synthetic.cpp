#include "nanotrade/synthetic.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/math.hpp"
#include "nanotrade/random.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace nanotrade {

BarSeries synthesize(const SyntheticSpec& spec) {
    if (spec.bars < 50 || spec.bars > 20'000) {
        throw Error("a synthetic market has from 50 to 20,000 bars", "data.synthetic.bars");
    }
    if (!(spec.drift >= -1.0 && spec.drift <= 1.0)) {
        throw Error("the drift must be from -100% to 100% a year", "data.synthetic.drift");
    }
    if (!(spec.volatility > 0.0 && spec.volatility <= 2.0)) {
        throw Error("the volatility must be above 0 and at most 200% a year", "data.synthetic.volatility");
    }
    if (!(spec.start_price > 0.0 && spec.start_price <= 1e6)) {
        throw Error("the start price must be positive", "data.synthetic.start_price");
    }

    constexpr double trading_days = 252.0;
    constexpr double alpha = 0.08;  // GARCH(1,1): today's variance reacts to yesterday's shock...
    constexpr double beta = 0.90;   // ...and remembers yesterday's variance
    const double long_run = spec.volatility * spec.volatility / trading_days;
    const double omega = long_run * (1.0 - alpha - beta);
    const double drift = (spec.drift - spec.volatility * spec.volatility / 2.0) / trading_days;

    Random random(spec.seed);
    BarSeries series;
    series.symbol = "SYNTH-" + std::to_string(spec.seed);
    series.reserve(spec.bars);
    std::int64_t day = days_from_civil(2000, 1, 3);
    double variance = long_run;
    double previous = spec.start_price;
    for (std::size_t i = 0; i < spec.bars; ++i) {
        const double sigma = std::sqrt(variance);
        // A jump on 1% of days, three times a normal shock; dividing by the mixture's standard deviation keeps the
        // shock's variance at 1, so the long-run volatility is the one asked for.
        double shock = random.normal();
        if (random.uniform() < 0.01) {
            shock += 3.0 * random.normal();
        }
        shock /= std::sqrt(1.09);
        const double log_return = drift + sigma * shock;
        const double open = previous * math::exp(0.3 * sigma * random.normal());
        const double close = previous * math::exp(log_return);
        const double high = std::max(open, close) * math::exp(0.6 * sigma * std::fabs(random.normal()));
        const double low = std::min(open, close) * math::exp(-0.6 * sigma * std::fabs(random.normal()));
        const double volume =
            std::round(1e6 * math::exp(0.3 * random.normal()) * (1.0 + std::fabs(log_return) / std::sqrt(long_run)));
        series.push_back(day * seconds_per_day, open, high, low, close, volume);

        variance = omega + alpha * (sigma * shock) * (sigma * shock) + beta * variance;
        previous = close;
        do {
            ++day;
        } while ((day + 4) % 7 == 0 || (day + 4) % 7 == 6);  // 1970-01-01 was a Thursday: skip Sundays and Saturdays
    }
    return series;
}

}  // namespace nanotrade
