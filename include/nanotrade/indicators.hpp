#ifndef NANOTRADE_INDICATORS_HPP
#define NANOTRADE_INDICATORS_HPP

#include "nanotrade/series.hpp"

#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nanotrade::indicators {

using Series = std::vector<double>;

// An indicator is NaN until its window holds enough values, and wherever its window holds a NaN, so indicators can
// be computed over each other: an EMA of the MACD line starts once the line itself has started.
inline bool defined(double value) noexcept {
    return !std::isnan(value);
}

Series sma(std::span<const double> x, std::size_t period);
Series ema(std::span<const double> x, std::size_t period);    // seeded with the SMA of its first window
Series rsi(std::span<const double> x, std::size_t period);    // Wilder's smoothing
Series stdev(std::span<const double> x, std::size_t period);  // population standard deviation, as Bollinger used
Series highest(std::span<const double> x, std::size_t period);
Series lowest(std::span<const double> x, std::size_t period);
Series change(std::span<const double> x, std::size_t period);  // x[t] - x[t - period]
Series roc(std::span<const double> x, std::size_t period);     // x[t] / x[t - period] - 1
Series zscore(std::span<const double> x, std::size_t period);
Series lag(std::span<const double> x, std::size_t bars);
Series true_range(const BarSeries& bars);
Series atr(const BarSeries& bars, std::size_t period);  // Wilder's average true range

// Indicators computed once and shared, by a canonical name such as "sma(close,20)". A parameter sweep computes each
// distinct indicator once rather than once per backtest. Safe to use from several threads.
class Cache {
public:
    explicit Cache(const BarSeries& bars) : bars_(bars) {}

    const BarSeries& bars() const noexcept {
        return bars_;
    }

    // The series named `key`, computed by `make` the first time anyone asks for it.
    const Series& get(const std::string& key, const std::function<Series()>& make);

    // A price column by name: open, high, low, close, volume, or hl2 (the bar's midpoint).
    const Series& column(const std::string& name);

    std::size_t size() const;

private:
    // Not std::call_once: libstdc++ builds it on pthread_once outside Linux, which a throwing computation can't
    // leave safely. A failed computation leaves its entry empty, for the next caller to try again.
    struct Entry {
        std::mutex mutex;
        std::condition_variable done;
        bool computing{false};
        bool ready{false};
        Series values;
    };

    const BarSeries& bars_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
};

}  // namespace nanotrade::indicators

#endif
