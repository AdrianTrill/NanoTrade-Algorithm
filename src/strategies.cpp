#include "nanotrade/strategies.h"

#include <stdexcept>

#include "nanotrade/indicators.h"

namespace nanotrade {

std::string toString(Signal signal) {
    switch (signal) {
        case Signal::Flat:
            return "flat";
        case Signal::Long:
            return "long";
    }

    return "unknown";
}

SmaCrossoverStrategy::SmaCrossoverStrategy(std::size_t short_window, std::size_t long_window)
    : short_window_(short_window), long_window_(long_window) {
    if (short_window_ == 0 || long_window_ == 0) {
        throw std::invalid_argument("SMA windows must be greater than zero.");
    }
    if (short_window_ >= long_window_) {
        throw std::invalid_argument("SMA short window must be smaller than the long window.");
    }
}

std::string SmaCrossoverStrategy::name() const {
    return "sma_crossover";
}

std::vector<Signal> SmaCrossoverStrategy::generateSignals(const std::vector<Bar>& bars) const {
    const auto short_sma = indicators::simpleMovingAverage(bars, short_window_);
    const auto long_sma = indicators::simpleMovingAverage(bars, long_window_);

    std::vector<Signal> signals(bars.size(), Signal::Flat);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (indicators::isDefined(short_sma[i]) && indicators::isDefined(long_sma[i]) && short_sma[i] > long_sma[i]) {
            signals[i] = Signal::Long;
        }
    }

    return signals;
}

EmaCrossoverStrategy::EmaCrossoverStrategy(std::size_t short_window, std::size_t long_window)
    : short_window_(short_window), long_window_(long_window) {
    if (short_window_ == 0 || long_window_ == 0) {
        throw std::invalid_argument("EMA windows must be greater than zero.");
    }
    if (short_window_ >= long_window_) {
        throw std::invalid_argument("EMA short window must be smaller than the long window.");
    }
}

std::string EmaCrossoverStrategy::name() const {
    return "ema_crossover";
}

std::vector<Signal> EmaCrossoverStrategy::generateSignals(const std::vector<Bar>& bars) const {
    const auto short_ema = indicators::exponentialMovingAverage(bars, short_window_);
    const auto long_ema = indicators::exponentialMovingAverage(bars, long_window_);

    std::vector<Signal> signals(bars.size(), Signal::Flat);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (indicators::isDefined(short_ema[i]) && indicators::isDefined(long_ema[i]) && short_ema[i] > long_ema[i]) {
            signals[i] = Signal::Long;
        }
    }

    return signals;
}

MomentumStrategy::MomentumStrategy(std::size_t lookback) : lookback_(lookback) {
    if (lookback_ == 0) {
        throw std::invalid_argument("Momentum lookback must be greater than zero.");
    }
}

std::string MomentumStrategy::name() const {
    return "momentum";
}

std::vector<Signal> MomentumStrategy::generateSignals(const std::vector<Bar>& bars) const {
    const auto momentum_values = indicators::momentum(bars, lookback_);

    std::vector<Signal> signals(bars.size(), Signal::Flat);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (indicators::isDefined(momentum_values[i]) && momentum_values[i] > 0.0) {
            signals[i] = Signal::Long;
        }
    }

    return signals;
}

RsiThresholdStrategy::RsiThresholdStrategy(std::size_t period, double entry_threshold, double exit_threshold)
    : period_(period), entry_threshold_(entry_threshold), exit_threshold_(exit_threshold) {
    if (period_ == 0) {
        throw std::invalid_argument("RSI period must be greater than zero.");
    }
    if (entry_threshold_ < 0.0 || exit_threshold_ > 100.0 || entry_threshold_ >= exit_threshold_) {
        throw std::invalid_argument("RSI thresholds must satisfy 0 <= entry < exit <= 100.");
    }
}

std::string RsiThresholdStrategy::name() const {
    return "rsi_threshold";
}

std::vector<Signal> RsiThresholdStrategy::generateSignals(const std::vector<Bar>& bars) const {
    const auto rsi_values = indicators::relativeStrengthIndex(bars, period_);

    std::vector<Signal> signals(bars.size(), Signal::Flat);
    Signal current_signal = Signal::Flat;

    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (indicators::isDefined(rsi_values[i])) {
            if (current_signal == Signal::Flat && rsi_values[i] <= entry_threshold_) {
                current_signal = Signal::Long;
            } else if (current_signal == Signal::Long && rsi_values[i] >= exit_threshold_) {
                current_signal = Signal::Flat;
            }
        }

        signals[i] = current_signal;
    }

    return signals;
}

}  // namespace nanotrade
