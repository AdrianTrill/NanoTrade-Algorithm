#include "nanotrade/indicators.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace nanotrade::indicators {
namespace {

double undefinedValue() {
    return std::numeric_limits<double>::quiet_NaN();
}

double computeRsi(double average_gain, double average_loss) {
    if (average_gain == 0.0 && average_loss == 0.0) {
        return 50.0;
    }
    if (average_loss == 0.0) {
        return 100.0;
    }
    if (average_gain == 0.0) {
        return 0.0;
    }

    const double relative_strength = average_gain / average_loss;
    return 100.0 - (100.0 / (1.0 + relative_strength));
}

}  // namespace

bool isDefined(double value) {
    return !std::isnan(value);
}

Series simpleMovingAverage(const std::vector<Bar>& bars, std::size_t period) {
    if (period == 0) {
        throw std::invalid_argument("SMA period must be greater than zero.");
    }

    Series result(bars.size(), undefinedValue());
    if (bars.empty()) {
        return result;
    }

    double rolling_sum = 0.0;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        rolling_sum += bars[i].close;
        if (i >= period) {
            rolling_sum -= bars[i - period].close;
        }

        if (i + 1 >= period) {
            result[i] = rolling_sum / static_cast<double>(period);
        }
    }

    return result;
}

Series exponentialMovingAverage(const std::vector<Bar>& bars, std::size_t period) {
    if (period == 0) {
        throw std::invalid_argument("EMA period must be greater than zero.");
    }

    Series result(bars.size(), undefinedValue());
    if (bars.size() < period) {
        return result;
    }

    double seed_sum = 0.0;
    for (std::size_t i = 0; i < period; ++i) {
        seed_sum += bars[i].close;
    }

    const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
    double ema = seed_sum / static_cast<double>(period);
    result[period - 1] = ema;

    for (std::size_t i = period; i < bars.size(); ++i) {
        ema = (bars[i].close * alpha) + (ema * (1.0 - alpha));
        result[i] = ema;
    }

    return result;
}

Series momentum(const std::vector<Bar>& bars, std::size_t period) {
    if (period == 0) {
        throw std::invalid_argument("Momentum period must be greater than zero.");
    }

    Series result(bars.size(), undefinedValue());
    for (std::size_t i = period; i < bars.size(); ++i) {
        result[i] = bars[i].close - bars[i - period].close;
    }

    return result;
}

Series relativeStrengthIndex(const std::vector<Bar>& bars, std::size_t period) {
    if (period == 0) {
        throw std::invalid_argument("RSI period must be greater than zero.");
    }

    Series result(bars.size(), undefinedValue());
    if (bars.size() <= period) {
        return result;
    }

    double gain_sum = 0.0;
    double loss_sum = 0.0;

    for (std::size_t i = 1; i <= period; ++i) {
        const double change = bars[i].close - bars[i - 1].close;
        if (change > 0.0) {
            gain_sum += change;
        } else {
            loss_sum += -change;
        }
    }

    double average_gain = gain_sum / static_cast<double>(period);
    double average_loss = loss_sum / static_cast<double>(period);
    result[period] = computeRsi(average_gain, average_loss);

    for (std::size_t i = period + 1; i < bars.size(); ++i) {
        const double change = bars[i].close - bars[i - 1].close;
        const double gain = change > 0.0 ? change : 0.0;
        const double loss = change < 0.0 ? -change : 0.0;

        average_gain = ((average_gain * (static_cast<double>(period) - 1.0)) + gain) / static_cast<double>(period);
        average_loss = ((average_loss * (static_cast<double>(period) - 1.0)) + loss) / static_cast<double>(period);
        result[i] = computeRsi(average_gain, average_loss);
    }

    return result;
}

}  // namespace nanotrade::indicators
