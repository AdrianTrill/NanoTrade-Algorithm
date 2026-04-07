#ifndef NANOTRADE_STRATEGIES_H
#define NANOTRADE_STRATEGIES_H

#include <cstddef>
#include <string>
#include <vector>

#include "nanotrade/market_data.h"

namespace nanotrade {

enum class Signal {
    Flat,
    Long
};

std::string toString(Signal signal);

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual std::string name() const = 0;
    virtual std::vector<Signal> generateSignals(const std::vector<Bar>& bars) const = 0;
};

class SmaCrossoverStrategy final : public Strategy {
public:
    SmaCrossoverStrategy(std::size_t short_window, std::size_t long_window);

    std::string name() const override;
    std::vector<Signal> generateSignals(const std::vector<Bar>& bars) const override;

private:
    std::size_t short_window_;
    std::size_t long_window_;
};

class EmaCrossoverStrategy final : public Strategy {
public:
    EmaCrossoverStrategy(std::size_t short_window, std::size_t long_window);

    std::string name() const override;
    std::vector<Signal> generateSignals(const std::vector<Bar>& bars) const override;

private:
    std::size_t short_window_;
    std::size_t long_window_;
};

class MomentumStrategy final : public Strategy {
public:
    explicit MomentumStrategy(std::size_t lookback);

    std::string name() const override;
    std::vector<Signal> generateSignals(const std::vector<Bar>& bars) const override;

private:
    std::size_t lookback_;
};

class RsiThresholdStrategy final : public Strategy {
public:
    RsiThresholdStrategy(std::size_t period, double entry_threshold, double exit_threshold);

    std::string name() const override;
    std::vector<Signal> generateSignals(const std::vector<Bar>& bars) const override;

private:
    std::size_t period_;
    double entry_threshold_;
    double exit_threshold_;
};

}  // namespace nanotrade

#endif
