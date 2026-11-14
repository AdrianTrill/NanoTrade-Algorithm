#include "nanotrade/indicators.hpp"

#include "nanotrade/error.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <utility>

namespace nanotrade::indicators {
namespace {

constexpr double undefined = std::numeric_limits<double>::quiet_NaN();

void require_period(std::size_t period, const char* name) {
    if (period == 0) {
        throw Error(std::string(name) + " needs a period of at least 1");
    }
}

// Exponential smoothing with factor alpha, restarted after any NaN, and seeded with the mean of `period` values.
Series smooth(std::span<const double> x, std::size_t period, double alpha) {
    Series out(x.size(), undefined);
    std::size_t run = 0;
    double seed = 0.0;
    double value = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!defined(x[i])) {
            run = 0;
            seed = 0.0;
            continue;
        }
        if (run < period) {
            seed += x[i];
            ++run;
            if (run == period) {
                value = seed / static_cast<double>(period);
                out[i] = value;
            }
            continue;
        }
        value += alpha * (x[i] - value);
        out[i] = value;
    }
    return out;
}

// The monotonic-queue rolling extreme: O(n) for any period.
template <typename Better>
Series rolling_extreme(std::span<const double> x, std::size_t period, Better better) {
    Series out(x.size(), undefined);
    std::deque<std::size_t> window;
    std::size_t last_nan = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!defined(x[i])) {
            last_nan = i;
            window.clear();
            continue;
        }
        while (!window.empty() && !better(x[window.back()], x[i])) {
            window.pop_back();
        }
        window.push_back(i);
        if (window.front() + period <= i) {
            window.pop_front();
        }
        const bool full =
            i + 1 >= period && (last_nan == std::numeric_limits<std::size_t>::max() || last_nan + period <= i);
        if (full) {
            out[i] = x[window.front()];
        }
    }
    return out;
}

}  // namespace

Series sma(std::span<const double> x, std::size_t period) {
    require_period(period, "sma");
    Series out(x.size(), undefined);
    // A running sum, with the rounding error of every addition kept apart and added back (Neumaier's compensated
    // summation): the mean stays within about an ulp of the window's exact one, however long the series runs.
    double sum = 0.0;
    double compensation = 0.0;
    const auto add = [&](double value) {
        const double total = sum + value;
        compensation += std::abs(sum) >= std::abs(value) ? (sum - total) + value : (value - total) + sum;
        sum = total;
    };
    std::size_t missing = 0;
    std::size_t equal_run = 0;  // how many values in a row, ending here, are the same
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (defined(x[i])) {
            add(x[i]);
            equal_run = i > 0 && x[i] == x[i - 1] ? equal_run + 1 : 1;
        } else {
            ++missing;
            equal_run = 0;
        }
        if (i >= period) {
            if (defined(x[i - period])) {
                add(-x[i - period]);
            } else {
                --missing;
            }
        }
        if (i + 1 >= period && missing == 0) {
            // The mean of equal prices is that price exactly, so a close can't sit a rounding error above its own
            // flat average.
            out[i] = equal_run >= period ? x[i] : (sum + compensation) / static_cast<double>(period);
        }
    }
    return out;
}

Series ema(std::span<const double> x, std::size_t period) {
    require_period(period, "ema");
    return smooth(x, period, 2.0 / (static_cast<double>(period) + 1.0));
}

Series rsi(std::span<const double> x, std::size_t period) {
    require_period(period, "rsi");
    Series out(x.size(), undefined);
    const auto value = [](double gain, double loss) {
        if (gain == 0.0 && loss == 0.0) {
            return 50.0;
        }
        return 100.0 - 100.0 / (1.0 + gain / loss);  // loss == 0 gives 100
    };
    const auto n = static_cast<double>(period);
    std::size_t run = 0;
    double gain = 0.0;
    double loss = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (!defined(x[i]) || !defined(x[i - 1])) {
            run = 0;
            gain = 0.0;
            loss = 0.0;
            continue;
        }
        const double delta = x[i] - x[i - 1];
        const double up = delta > 0.0 ? delta : 0.0;
        const double down = delta < 0.0 ? -delta : 0.0;
        if (run < period) {
            gain += up;
            loss += down;
            ++run;
            if (run == period) {
                gain /= n;
                loss /= n;
                out[i] = value(gain, loss);
            }
            continue;
        }
        gain = (gain * (n - 1.0) + up) / n;
        loss = (loss * (n - 1.0) + down) / n;
        out[i] = value(gain, loss);
    }
    return out;
}

namespace {

// The mean and population standard deviation of the window ending at each bar, by two passes over the window.
// Values are measured from the window's first one, so a flat window has exactly no deviation, and the deviations of
// a two-bar window are exactly opposite. O(n * period), which beats running sums on accuracy.
struct WindowStats {
    Series offset;  // the window's first value
    Series mean;    // the mean, less the offset
    Series deviation;
};

WindowStats window_stats(std::span<const double> x, std::size_t period) {
    WindowStats stats{Series(x.size(), undefined), Series(x.size(), undefined), Series(x.size(), undefined)};
    const auto n = static_cast<double>(period);
    std::size_t defined_run = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        defined_run = defined(x[i]) ? defined_run + 1 : 0;
        if (defined_run < period) {
            continue;
        }
        const std::size_t first = i + 1 - period;
        const double offset = x[first];
        double sum = 0.0;
        for (std::size_t j = first; j <= i; ++j) {
            sum += x[j] - offset;
        }
        const double mean = sum / n;
        double squares = 0.0;
        for (std::size_t j = first; j <= i; ++j) {
            const double d = (x[j] - offset) - mean;
            squares += d * d;
        }
        stats.offset[i] = offset;
        stats.mean[i] = mean;
        stats.deviation[i] = std::sqrt(squares / n);
    }
    return stats;
}

}  // namespace

Series stdev(std::span<const double> x, std::size_t period) {
    require_period(period, "stdev");
    return window_stats(x, period).deviation;
}

Series highest(std::span<const double> x, std::size_t period) {
    require_period(period, "highest");
    return rolling_extreme(x, period, [](double kept, double incoming) { return kept > incoming; });
}

Series lowest(std::span<const double> x, std::size_t period) {
    require_period(period, "lowest");
    return rolling_extreme(x, period, [](double kept, double incoming) { return kept < incoming; });
}

Series change(std::span<const double> x, std::size_t period) {
    require_period(period, "change");
    Series out(x.size(), undefined);
    for (std::size_t i = period; i < x.size(); ++i) {
        out[i] = x[i] - x[i - period];
    }
    return out;
}

Series roc(std::span<const double> x, std::size_t period) {
    require_period(period, "roc");
    Series out(x.size(), undefined);
    for (std::size_t i = period; i < x.size(); ++i) {
        if (x[i - period] != 0.0) {
            out[i] = x[i] / x[i - period] - 1.0;
        }
    }
    return out;
}

Series zscore(std::span<const double> x, std::size_t period) {
    require_period(period, "zscore");
    const WindowStats stats = window_stats(x, period);
    Series out(x.size(), undefined);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (defined(stats.deviation[i]) && stats.deviation[i] > 0.0) {
            out[i] = ((x[i] - stats.offset[i]) - stats.mean[i]) / stats.deviation[i];
        }
    }
    return out;
}

Series lag(std::span<const double> x, std::size_t bars) {
    Series out(x.size(), undefined);
    for (std::size_t i = bars; i < x.size(); ++i) {
        out[i] = x[i - bars];
    }
    return out;
}

Series true_range(const BarSeries& bars) {
    Series out(bars.size(), undefined);
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const double previous = bars.close[i - 1];
        // std::max would quietly drop a NaN, so a missing price makes the range unknown explicitly.
        if (!defined(previous) || !defined(bars.high[i]) || !defined(bars.low[i])) {
            continue;
        }
        out[i] = std::max(
            {bars.high[i] - bars.low[i], std::fabs(bars.high[i] - previous), std::fabs(bars.low[i] - previous)});
    }
    return out;
}

Series atr(const BarSeries& bars, std::size_t period) {
    require_period(period, "atr");
    const Series range = true_range(bars);
    return smooth(range, period, 1.0 / static_cast<double>(period));
}

const Series& Cache::get(const std::string& key, const std::function<Series()>& make) {
    std::shared_ptr<Entry> entry;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::shared_ptr<Entry>& slot = entries_[key];
        if (!slot) {
            slot = std::make_shared<Entry>();
        }
        entry = slot;
    }
    std::unique_lock<std::mutex> lock(entry->mutex);
    entry->done.wait(lock, [&] { return !entry->computing; });
    if (entry->ready) {
        return entry->values;
    }
    entry->computing = true;
    lock.unlock();
    // Computed outside every lock, so one indicator can ask the cache for others while it is being computed.
    Series values;
    try {
        values = make();
    } catch (...) {
        lock.lock();
        entry->computing = false;
        lock.unlock();
        entry->done.notify_all();
        throw;
    }
    lock.lock();
    entry->values = std::move(values);
    entry->ready = true;
    entry->computing = false;
    lock.unlock();
    entry->done.notify_all();
    return entry->values;
}

const Series& Cache::column(const std::string& name) {
    return get(name, [&]() -> Series {
        if (name == "open") {
            return bars_.open;
        }
        if (name == "high") {
            return bars_.high;
        }
        if (name == "low") {
            return bars_.low;
        }
        if (name == "close") {
            return bars_.close;
        }
        if (name == "volume") {
            return bars_.volume;
        }
        if (name == "hl2") {
            Series out(bars_.size());
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = (bars_.high[i] + bars_.low[i]) / 2.0;
            }
            return out;
        }
        throw Error("unknown price column " + name);
    });
}

std::size_t Cache::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace nanotrade::indicators
