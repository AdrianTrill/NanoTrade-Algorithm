#include "nanotrade/portfolio.hpp"

#include "nanotrade/error.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>

namespace nanotrade {
namespace {

constexpr double undefined = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t none = static_cast<std::size_t>(-1);

// The dates every market traded, oldest first.
std::vector<Timestamp> common_dates(std::span<const PortfolioMarket> markets) {
    std::vector<Timestamp> dates = markets.front().bars->time;
    for (std::size_t m = 1; m < markets.size() && !dates.empty(); ++m) {
        const std::vector<Timestamp>& other = markets[m].bars->time;
        std::vector<Timestamp> shared;
        std::set_intersection(dates.begin(), dates.end(), other.begin(), other.end(), std::back_inserter(shared));
        dates = std::move(shared);
    }
    return dates;
}

// Each date's bar in a series that has every one of them.
std::vector<std::size_t> locate(const std::vector<Timestamp>& dates, const BarSeries& bars) {
    std::vector<std::size_t> index(dates.size());
    std::size_t bar = 0;
    for (std::size_t i = 0; i < dates.size(); ++i) {
        while (bars.time[bar] < dates[i]) {
            ++bar;
        }
        index[i] = bar;
    }
    return index;
}

// Whether a rebalancing period ends with the close on `today`, the next close being on `next`.
bool period_ends(Rebalance rebalance, Timestamp today, Timestamp next) {
    if (rebalance == Rebalance::Never) {
        return false;
    }
    const CivilDate a = civil_from_timestamp(today);
    const CivilDate b = civil_from_timestamp(next);
    if (a.year != b.year) {
        return true;
    }
    if (rebalance == Rebalance::Quarterly) {
        return (a.month - 1) / 3 != (b.month - 1) / 3;
    }
    return rebalance == Rebalance::Monthly && a.month != b.month;
}

// The sample standard deviation of a market's last `window` close-to-close returns, to its close at bar `last`; NaN
// with fewer than two returns to measure.
double recent_volatility(const BarSeries& bars, std::size_t last, std::size_t window) {
    if (last == none) {
        return undefined;
    }
    const std::size_t count = std::min(window, last);
    if (count < 2) {
        return undefined;
    }
    const auto change = [&](std::size_t i) {
        return bars.close[i] / bars.close[i - 1] - 1.0;
    };
    double mean = 0.0;
    for (std::size_t i = last + 1 - count; i <= last; ++i) {
        mean += change(i);
    }
    mean /= static_cast<double>(count);
    double squares = 0.0;
    for (std::size_t i = last + 1 - count; i <= last; ++i) {
        const double deviation = change(i) - mean;
        squares += deviation * deviation;
    }
    return std::sqrt(squares / static_cast<double>(count - 1));
}

// The target weights at a close, given each market's bar at that close (`none` before its first). A sleeve whose
// backtest was ruined gets nothing; inverse-volatility weights stay equal until every market has returns to measure.
std::vector<double> target_weights(const PortfolioSpec& spec,
                                   std::span<const PortfolioMarket> markets,
                                   const std::vector<std::size_t>& last,
                                   const std::vector<bool>& alive) {
    const std::size_t n = markets.size();
    std::vector<double> weights(n, 1.0);
    if (spec.weighting == Weighting::Fixed) {
        weights = spec.weights;
    } else if (spec.weighting == Weighting::InverseVolatility) {
        std::vector<double> inverse(n);
        bool measured = true;
        for (std::size_t i = 0; i < n; ++i) {
            const double volatility = recent_volatility(*markets[i].bars, last[i], spec.volatility_window);
            measured = measured && volatility > 0.0;
            inverse[i] = 1.0 / volatility;
        }
        if (measured) {
            weights = std::move(inverse);
        }
    }
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!alive[i]) {
            weights[i] = 0.0;
        }
        total += weights[i];
    }
    for (double& weight : weights) {
        weight = total > 0.0 ? weight / total : 0.0;
    }
    return weights;
}

// Where the markets' bars fall on the calendar.
struct Layout {
    const std::vector<Timestamp>& dates;
    const std::vector<std::vector<std::size_t>>& at;  // [market][calendar bar]: the market's bar on that date
    std::size_t begin;
    std::size_t end;
};

// The sleeves combined into one portfolio, close by close.
struct Track {
    std::vector<double> equity;               // the portfolio at each close of the window
    std::vector<double> held;                 // the value held in the markets, long or short
    std::vector<double> exposure;             // the net value held, as a fraction of the portfolio
    std::vector<std::vector<double>> weight;  // [sleeve][bar]: its share of the portfolio
    std::vector<std::vector<double>> scale;   // [sleeve][bar]: its value per unit of its backtest's equity
    std::vector<double> targets;              // the first target weights
    std::vector<double> contribution;         // each sleeve's gain from its own trading, in money
    std::size_t rebalances{0};
    std::size_t trades{0};  // rebalancing trades, one for each market whose holdings changed
    double traded{0.0};
    double commissions{0.0};
    double slippage{0.0};
    bool ruined{false};
};

Track combine(std::span<const BacktestResult* const> sleeves,
              std::span<const PortfolioMarket> markets,
              const Layout& layout,
              const BacktestConfig& config,
              const PortfolioSpec& spec) {
    const std::size_t n = sleeves.size();
    const std::size_t length = layout.end - layout.begin;
    const auto bar_of = [&](std::size_t i, std::size_t c) {
        return layout.at[i][c] - sleeves[i]->begin;
    };
    Track track;
    track.equity.resize(length);
    track.held.resize(length);
    track.exposure.resize(length);
    track.weight.assign(n, std::vector<double>(length));
    track.scale.assign(n, std::vector<double>(length));

    // The first targets, from what each market showed up to its close before the window.
    std::vector<std::size_t> last(n);
    std::vector<bool> alive(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        last[i] = layout.at[i][layout.begin] > 0 ? layout.at[i][layout.begin] - 1 : none;
    }
    track.targets = target_weights(spec, markets, last, alive);
    // Each sleeve's backtest starts from the whole capital, so its first scale is its weight.
    std::vector<double> scale = track.targets;
    std::vector<double> value(n);
    std::vector<double> moved_in(n, 0.0);  // capital each sleeve received from rebalancing, net
    for (std::size_t k = 0; k < length; ++k) {
        const std::size_t c = layout.begin + k;
        double total = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            track.scale[i][k] = scale[i];
            value[i] = sleeves[i]->equity[bar_of(i, c)] * scale[i];
            total += value[i];
        }
        if (!(total > 0.0)) {
            track.ruined = true;
        }
        // Rebalance at the close that ends a period, except the last close.
        if (k + 1 < length && total > 0.0 && period_ends(spec.rebalance, layout.dates[c], layout.dates[c + 1])) {
            for (std::size_t i = 0; i < n; ++i) {
                last[i] = layout.at[i][c];
                alive[i] = sleeves[i]->equity[bar_of(i, c)] > 0.0;
            }
            const std::vector<double> target = target_weights(spec, markets, last, alive);
            double cost = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                // A sleeve given or relieved of capital buys or sells its market in proportion to what it holds.
                const double moved =
                    std::fabs(target[i] * total - value[i]) * std::fabs(sleeves[i]->exposure[bar_of(i, c)]);
                if (moved > total * 1e-12) {
                    const double commission = moved * config.commission_bps / 10'000.0 + config.commission_fixed;
                    const double slippage = moved * config.slippage_bps / 10'000.0;
                    cost += commission + slippage;
                    track.commissions += commission;
                    track.slippage += slippage;
                    track.traded += moved;
                    ++track.trades;
                }
            }
            const double after = total - cost;
            for (std::size_t i = 0; i < n; ++i) {
                const double next = target[i] * after;
                moved_in[i] += next - value[i];
                scale[i] = alive[i] ? next / sleeves[i]->equity[bar_of(i, c)] : 0.0;
                value[i] = next;
            }
            total = after;
            ++track.rebalances;
        }
        double held = 0.0;
        double net = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double exposure = sleeves[i]->exposure[bar_of(i, c)];
            held += std::fabs(value[i] * exposure);
            net += value[i] * exposure;
            track.weight[i][k] = total > 0.0 ? value[i] / total : 0.0;
        }
        track.equity[k] = std::max(total, 0.0);
        track.held[k] = held;
        track.exposure[k] = total > 0.0 ? net / total : 0.0;
    }
    track.contribution.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        track.contribution[i] = value[i] - track.targets[i] * config.initial_capital - moved_in[i];
    }
    return track;
}

// The portfolio as a backtest on the calendar: its equity, the sleeves' trades pooled with their profits scaled to the
// sleeves' sizes, and every cost, so the measures of a single backtest apply to it.
BacktestResult assemble(const Track& track,
                        std::span<const BacktestResult* const> sleeves,
                        std::span<const PortfolioMarket> markets,
                        const Layout& layout,
                        const BacktestConfig& config,
                        double bars_per_year) {
    BacktestResult result;
    result.begin = layout.begin;
    result.end = layout.end;
    result.initial_capital = config.initial_capital;
    result.bars_per_year = bars_per_year;
    result.equity = track.equity;
    result.position = track.held;
    result.exposure = track.exposure;
    result.commissions = track.commissions;
    result.slippage = track.slippage;
    result.traded_value = track.traded;
    result.ruined = track.ruined;
    const auto first = layout.dates.begin() + static_cast<std::ptrdiff_t>(layout.begin);
    const auto last = layout.dates.begin() + static_cast<std::ptrdiff_t>(layout.end);
    for (std::size_t i = 0; i < sleeves.size(); ++i) {
        const BacktestResult& sleeve = *sleeves[i];
        const BarSeries& bars = *markets[i].bars;
        // A market's bar on the calendar: its date's, or the next shared date's when the other markets didn't trade.
        const auto calendar_bar = [&](std::size_t bar) {
            const auto found = std::lower_bound(first, last, bars.time[bar]);
            return std::min(static_cast<std::size_t>(found - layout.dates.begin()), layout.end - 1);
        };
        const auto scale = [&](std::size_t bar) {
            return track.scale[i][calendar_bar(bar) - layout.begin];
        };
        for (const Fill& fill : sleeve.fills) {
            const double s = scale(fill.bar);
            result.commissions += fill.commission * s;
            result.slippage += fill.slippage * s;
            result.traded_value += std::fabs(fill.shares) * fill.price * s;
        }
        if (config.borrow_rate > 0.0) {
            for (std::size_t k = 0; k < sleeve.position.size(); ++k) {
                if (sleeve.position[k] < 0.0) {
                    const std::size_t bar = sleeve.begin + k;
                    result.borrow +=
                        -sleeve.position[k] * bars.close[bar] * config.borrow_rate / sleeve.bars_per_year * scale(bar);
                }
            }
        }
        for (const Trade& trade : sleeve.trades) {
            Trade pooled = trade;
            const double s = scale(trade.exit_bar);
            pooled.entry_bar = calendar_bar(trade.entry_bar);
            pooled.exit_bar = calendar_bar(trade.exit_bar);
            pooled.shares *= s;
            pooled.pnl *= s;
            result.trades.push_back(pooled);
        }
        result.trimmed += sleeve.trimmed;
    }
    std::stable_sort(result.trades.begin(), result.trades.end(), [](const Trade& a, const Trade& b) {
        return a.exit_bar != b.exit_bar ? a.exit_bar < b.exit_bar : a.entry_bar < b.entry_bar;
    });
    return result;
}

// The correlations of the sleeves' daily returns on the calendar.
std::vector<std::vector<double>> correlations(std::span<const BacktestResult* const> sleeves, const Layout& layout) {
    const std::size_t n = sleeves.size();
    const std::size_t length = layout.end - layout.begin;
    std::vector<std::vector<double>> returns(n, std::vector<double>(length));
    std::vector<double> mean(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double previous = sleeves[i]->initial_capital;
        for (std::size_t k = 0; k < length; ++k) {
            const double equity = sleeves[i]->equity[layout.at[i][layout.begin + k] - sleeves[i]->begin];
            returns[i][k] = previous > 0.0 ? equity / previous - 1.0 : 0.0;
            previous = equity;
            mean[i] += returns[i][k];
        }
        mean[i] /= static_cast<double>(length);
    }
    const auto co = [&](std::size_t a, std::size_t b) {
        double sum = 0.0;
        for (std::size_t k = 0; k < length; ++k) {
            sum += (returns[a][k] - mean[a]) * (returns[b][k] - mean[b]);
        }
        return sum;
    };
    std::vector<double> variance(n);
    for (std::size_t i = 0; i < n; ++i) {
        variance[i] = co(i, i);
    }
    std::vector<std::vector<double>> matrix(n, std::vector<double>(n, undefined));
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = a; b < n; ++b) {
            if (variance[a] > 0.0 && variance[b] > 0.0) {
                const double r = a == b ? 1.0 : co(a, b) / std::sqrt(variance[a] * variance[b]);
                matrix[a][b] = matrix[b][a] = std::clamp(r, -1.0, 1.0);
            }
        }
    }
    return matrix;
}

}  // namespace

PortfolioResult run_portfolio(std::span<const PortfolioMarket> markets,
                              const StrategySpec& strategy,
                              const BacktestConfig& config,
                              const PortfolioSpec& spec,
                              std::optional<Timestamp> from,
                              std::optional<Timestamp> to) {
    validate(config);
    const std::size_t n = markets.size();
    if (n == 0 || n > max_portfolio_markets) {
        throw Error("a portfolio holds from 1 to " + std::to_string(max_portfolio_markets) + " markets",
                    "portfolio.markets");
    }
    if (spec.weighting == Weighting::Fixed) {
        if (spec.weights.size() != n) {
            throw Error("portfolio.weights needs a weight for each of the " + std::to_string(n) + " markets",
                        "portfolio.weights");
        }
        double total = 0.0;
        for (const double weight : spec.weights) {
            if (!(weight >= 0.0 && weight <= 1e6)) {
                throw Error("each weight must be a number from 0 to 1,000,000", "portfolio.weights");
            }
            total += weight;
        }
        if (!(total > 0.0)) {
            throw Error("at least one weight must be above zero", "portfolio.weights");
        }
    }
    if (spec.volatility_window < 2 || spec.volatility_window > 1000) {
        throw Error("portfolio.volatility_window must be from 2 to 1000 bars", "portfolio.volatility_window");
    }

    PortfolioResult out;
    const std::vector<Timestamp> dates = common_dates(markets);
    if (dates.size() < 2) {
        throw Error("the markets have fewer than two dates in common", "portfolio.markets");
    }
    const auto first = from ? std::lower_bound(dates.begin(), dates.end(), *from) : dates.begin();
    const auto last = to ? std::upper_bound(dates.begin(), dates.end(), *to) : dates.end();
    if (last - first < 2) {
        throw Error("the date range holds fewer than two of the dates the markets share, which run from " +
                        format_date(dates.front()) + " to " + format_date(dates.back()),
                    "range");
    }
    const auto begin = static_cast<std::size_t>(first - dates.begin());
    const auto end = static_cast<std::size_t>(last - dates.begin());
    out.calendar.symbol = "portfolio";
    out.calendar.time = dates;

    std::vector<std::vector<std::size_t>> at(n);
    out.sleeves.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        at[i] = locate(dates, *markets[i].bars);
        PortfolioSleeve& sleeve = out.sleeves[i];
        sleeve.begin = at[i][begin];
        sleeve.end = at[i][end - 1] + 1;
        sleeve.report = run_report(*markets[i].bars, *markets[i].cache, strategy, config, sleeve.begin, sleeve.end);
    }

    const Layout layout{dates, at, begin, end};
    const double bars_per_year = config.bars_per_year > 0.0 ? config.bars_per_year : out.calendar.bars_per_year();
    std::vector<const BacktestResult*> traded(n);
    std::vector<const BacktestResult*> held(n);
    for (std::size_t i = 0; i < n; ++i) {
        traded[i] = &out.sleeves[i].report.result;
        held[i] = &out.sleeves[i].report.benchmark;
    }
    const Track track = combine(traded, markets, layout, config, spec);
    const Track held_track = combine(held, markets, layout, config, spec);
    out.result = assemble(track, traded, markets, layout, config, bars_per_year);
    out.benchmark = assemble(held_track, held, markets, layout, config, bars_per_year);
    out.benchmark_metrics = compute_metrics(out.calendar, out.benchmark, {}, config.risk_free_rate);
    const std::vector<double> benchmark_returns = bar_returns(out.benchmark);
    out.metrics = compute_metrics(out.calendar, out.result, benchmark_returns, config.risk_free_rate);

    out.rebalances = track.rebalances;
    out.rebalance_traded = track.traded;
    out.rebalance_cost = track.commissions + track.slippage;
    out.fills = track.trades;
    for (std::size_t i = 0; i < n; ++i) {
        PortfolioSleeve& sleeve = out.sleeves[i];
        sleeve.target = track.targets[i];
        sleeve.weight = track.weight[i];
        sleeve.final_weight = sleeve.weight.back();
        sleeve.contribution = track.contribution[i] / config.initial_capital;
        out.fills += sleeve.report.result.fills.size();
    }
    out.correlation = correlations(traded, layout);
    return out;
}

}  // namespace nanotrade
