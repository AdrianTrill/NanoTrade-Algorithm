#include "nanotrade/metrics.hpp"

#include "nanotrade/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nanotrade {
namespace {

constexpr double undefined = std::numeric_limits<double>::quiet_NaN();

double ratio(double numerator, double denominator) {
    return denominator != 0.0 && std::isfinite(denominator) ? numerator / denominator : undefined;
}

// The p-th quantile with linear interpolation between order statistics (type 7, the default of R and NumPy). Selects
// the two order statistics it needs rather than sorting, since every trial of a sweep computes it.
double quantile(std::vector<double> values, double p) {
    if (values.empty()) {
        return undefined;
    }
    const double position = p * static_cast<double>(values.size() - 1);
    const auto below = static_cast<std::size_t>(std::floor(position));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(below), values.end());
    const double lower = values[below];
    if (below + 1 >= values.size()) {
        return lower;
    }
    const double upper = *std::min_element(values.begin() + static_cast<std::ptrdiff_t>(below) + 1, values.end());
    return lower + (upper - lower) * (position - static_cast<double>(below));
}

}  // namespace

std::vector<double> bar_returns(const BacktestResult& result) {
    std::vector<double> returns(result.equity.size());
    double previous = result.initial_capital;
    for (std::size_t i = 0; i < returns.size(); ++i) {
        returns[i] = previous > 0.0 ? result.equity[i] / previous - 1.0 : 0.0;
        previous = result.equity[i];
    }
    return returns;
}

ReturnMoments moments(std::span<const double> returns) {
    ReturnMoments m;
    m.count = returns.size();
    if (returns.empty()) {
        return m;
    }
    const auto n = static_cast<double>(returns.size());
    double sum = 0.0;
    for (const double r : returns) {
        sum += r;
    }
    m.mean = sum / n;
    double m2 = 0.0;
    double m3 = 0.0;
    double m4 = 0.0;
    for (const double r : returns) {
        const double d = r - m.mean;
        const double d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
        m4 += d2 * d2;
    }
    m.stdev = returns.size() > 1 ? std::sqrt(m2 / (n - 1.0)) : 0.0;
    const double variance = m2 / n;
    if (variance > 0.0) {
        m.skewness = (m3 / n) / (variance * std::sqrt(variance));
        m.kurtosis = (m4 / n) / (variance * variance) - 3.0;
    }
    return m;
}

double probabilistic_sharpe(double sharpe, double benchmark, std::size_t count, double skewness, double kurtosis) {
    if (count < 2 || !std::isfinite(sharpe)) {
        return undefined;
    }
    // Kurtosis enters as gamma_4 - 1, where gamma_4 is the plain (not excess) kurtosis.
    const double variance = 1.0 - skewness * sharpe + (kurtosis + 2.0) / 4.0 * sharpe * sharpe;
    if (!(variance > 0.0)) {
        return undefined;
    }
    return math::normal_cdf((sharpe - benchmark) * std::sqrt(static_cast<double>(count - 1)) / std::sqrt(variance));
}

std::vector<double> drawdown_series(std::span<const double> equity, double initial) {
    std::vector<double> out(equity.size());
    double peak = initial;
    for (std::size_t i = 0; i < equity.size(); ++i) {
        peak = std::max(peak, equity[i]);
        out[i] = peak > 0.0 ? equity[i] / peak - 1.0 : 0.0;
    }
    return out;
}

std::vector<DrawdownPeriod> worst_drawdowns(std::span<const double> equity, double initial, std::size_t count) {
    std::vector<DrawdownPeriod> periods;
    double peak = initial;
    std::size_t peak_index = 0;
    bool peak_is_initial = true;
    bool in_drawdown = false;
    DrawdownPeriod current;
    for (std::size_t i = 0; i < equity.size(); ++i) {
        if (equity[i] >= peak) {
            if (in_drawdown) {
                current.recovery = i;
                current.recovered = true;
                periods.push_back(current);
                in_drawdown = false;
            }
            peak = equity[i];
            peak_index = i;
            peak_is_initial = false;
            continue;
        }
        const double depth = 1.0 - equity[i] / peak;
        if (!in_drawdown) {
            in_drawdown = true;
            current = DrawdownPeriod{peak_index, i, equity.size(), false, depth, peak_is_initial};
        } else if (depth > current.depth) {
            current.depth = depth;
            current.trough = i;
        }
    }
    if (in_drawdown) {
        periods.push_back(current);
    }
    std::stable_sort(periods.begin(), periods.end(), [](const DrawdownPeriod& a, const DrawdownPeriod& b) {
        return a.depth > b.depth;
    });
    if (periods.size() > count) {
        periods.resize(count);
    }
    return periods;
}

DrawdownSpan drawdown_span(const DrawdownPeriod& period, std::size_t begin, std::size_t end) {
    DrawdownSpan span;
    span.first = period.from_initial && begin > 0 ? begin - 1 : begin + period.peak;
    span.last = period.recovered ? begin + period.recovery : end - 1;
    return span;
}

Metrics compute_metrics(const BarSeries& bars,
                        const BacktestResult& result,
                        std::span<const double> benchmark_returns,
                        double risk_free_rate) {
    Metrics m;
    const std::vector<double> returns = bar_returns(result);
    const auto n = returns.size();
    const double per_year = result.bars_per_year;
    const double initial = result.initial_capital;
    m.final_equity = result.equity.empty() ? initial : result.equity.back();
    m.total_return = m.final_equity / initial - 1.0;

    // Years from the calendar: the window runs from the close before its first bar (or that bar, at the very start)
    // to its last close.
    const Timestamp start = bars.time[result.begin > 0 ? result.begin - 1 : 0];
    m.years = years_between(start, bars.time[result.end - 1]);
    if (m.years <= 0.0) {
        m.years = static_cast<double>(n) / per_year;
    }
    m.cagr = m.final_equity > 0.0 ? math::pow(m.final_equity / initial, 1.0 / m.years) - 1.0 : -1.0;

    const ReturnMoments stats = moments(returns);
    const double rf = risk_free_rate / per_year;
    const double annualize = std::sqrt(per_year);
    m.volatility = stats.stdev * annualize;
    const double sharpe_per_bar = ratio(stats.mean - rf, stats.stdev);
    m.sharpe = sharpe_per_bar * annualize;
    double downside = 0.0;
    for (const double r : returns) {
        const double shortfall = std::min(r - rf, 0.0);
        downside += shortfall * shortfall;
    }
    downside = n > 0 ? std::sqrt(downside / static_cast<double>(n)) : 0.0;
    m.sortino = ratio(stats.mean - rf, downside) * annualize;
    m.skewness = stats.skewness;
    m.kurtosis = stats.kurtosis;
    m.psr = probabilistic_sharpe(sharpe_per_bar, 0.0, n, stats.skewness, stats.kurtosis);

    const std::vector<double> drawdowns = drawdown_series(result.equity, initial);
    for (const double d : drawdowns) {
        m.max_drawdown = std::max(m.max_drawdown, -d);
    }
    m.calmar = ratio(m.cagr, m.max_drawdown);
    for (const DrawdownPeriod& period : worst_drawdowns(result.equity, initial, n)) {
        const DrawdownSpan span = drawdown_span(period, result.begin, result.end);
        const std::size_t length = span.last - span.first;
        if (length > m.longest_drawdown_bars) {
            m.longest_drawdown_bars = length;
            m.longest_drawdown_days = static_cast<double>(bars.time[span.last] - bars.time[span.first]) /
                                      static_cast<double>(seconds_per_day);
        }
    }

    std::size_t held = 0;
    double equity_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        held += result.position[i] != 0.0 ? 1 : 0;
        equity_sum += result.equity[i];
    }
    m.exposure = n > 0 ? static_cast<double>(held) / static_cast<double>(n) : 0.0;
    const double average_equity = n > 0 ? equity_sum / static_cast<double>(n) : initial;
    m.turnover = ratio(result.traded_value / average_equity, m.years);
    m.costs = result.commissions + result.slippage + result.borrow;

    if (n > 0) {
        const double threshold = quantile(returns, 0.05);
        m.var_95 = -threshold;
        double tail = 0.0;
        std::size_t tail_count = 0;
        for (const double r : returns) {
            if (r <= threshold) {
                tail += r;
                ++tail_count;
            }
        }
        m.cvar_95 = tail_count > 0 ? -tail / static_cast<double>(tail_count) : undefined;
    }

    // Trades: closed ones only, so an open position's paper result doesn't count as a win or a loss.
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    double win_sum = 0.0;
    double loss_sum = 0.0;
    double return_sum = 0.0;
    double bars_sum = 0.0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    std::size_t streak = 0;
    m.best_trade = undefined;
    m.worst_trade = undefined;
    for (const Trade& trade : result.trades) {
        if (trade.open) {
            continue;
        }
        ++m.trades;
        return_sum += trade.return_pct;
        bars_sum += static_cast<double>(trade.exit_bar - trade.entry_bar);
        m.best_trade = std::isnan(m.best_trade) ? trade.return_pct : std::max(m.best_trade, trade.return_pct);
        m.worst_trade = std::isnan(m.worst_trade) ? trade.return_pct : std::min(m.worst_trade, trade.return_pct);
        if (trade.pnl > 0.0) {
            ++wins;
            gross_profit += trade.pnl;
            win_sum += trade.return_pct;
            streak = 0;
        } else {
            ++losses;
            gross_loss -= trade.pnl;
            loss_sum += trade.return_pct;
            m.max_losing_streak = std::max(m.max_losing_streak, ++streak);
        }
    }
    const auto trades = static_cast<double>(m.trades);
    m.win_rate = m.trades > 0 ? static_cast<double>(wins) / trades : undefined;
    m.profit_factor = ratio(gross_profit, gross_loss);
    m.avg_trade = m.trades > 0 ? return_sum / trades : undefined;
    m.avg_win = wins > 0 ? win_sum / static_cast<double>(wins) : undefined;
    m.avg_loss = losses > 0 ? loss_sum / static_cast<double>(losses) : undefined;
    m.avg_bars_held = m.trades > 0 ? bars_sum / trades : undefined;

    m.beta = undefined;
    m.alpha = undefined;
    m.correlation = undefined;
    if (benchmark_returns.size() == n && n > 1) {
        const ReturnMoments bench = moments(benchmark_returns);
        double covariance = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            covariance += (returns[i] - stats.mean) * (benchmark_returns[i] - bench.mean);
        }
        covariance /= static_cast<double>(n - 1);
        m.beta = ratio(covariance, bench.stdev * bench.stdev);
        m.alpha = (stats.mean - m.beta * bench.mean) * per_year;
        m.correlation = ratio(covariance, stats.stdev * bench.stdev);
    }
    return m;
}

std::vector<YearReturns> calendar_returns(const BarSeries& bars, const BacktestResult& result) {
    std::vector<YearReturns> years;
    double month_start = result.initial_capital;
    double year_start = result.initial_capital;
    for (std::size_t i = 0; i < result.equity.size(); ++i) {
        const CivilDate date = civil_from_timestamp(bars.time[result.begin + i]);
        const bool last = i + 1 == result.equity.size();
        CivilDate next{};
        if (!last) {
            next = civil_from_timestamp(bars.time[result.begin + i + 1]);
        }
        if (years.empty() || years.back().year != date.year) {
            YearReturns row;
            row.year = date.year;
            row.months.fill(undefined);
            years.push_back(row);
        }
        const double equity = result.equity[i];
        if (last || next.month != date.month || next.year != date.year) {
            years.back().months[date.month - 1] = equity / month_start - 1.0;
            month_start = equity;
        }
        if (last || next.year != date.year) {
            years.back().total = equity / year_start - 1.0;
            year_start = equity;
        }
    }
    return years;
}

}  // namespace nanotrade
