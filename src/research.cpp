#include "nanotrade/research.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/math.hpp"
#include "nanotrade/random.hpp"
#include "nanotrade/text.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

namespace nanotrade {
namespace {

constexpr double undefined = std::numeric_limits<double>::quiet_NaN();

BacktestConfig benchmark_config(const BacktestConfig& config) {
    BacktestConfig benchmark;
    benchmark.initial_capital = config.initial_capital;
    benchmark.commission_bps = config.commission_bps;
    benchmark.commission_fixed = config.commission_fixed;
    benchmark.slippage_bps = config.slippage_bps;
    benchmark.fractional_shares = config.fractional_shares;
    benchmark.close_at_end = config.close_at_end;
    benchmark.risk_free_rate = config.risk_free_rate;
    benchmark.bars_per_year = config.bars_per_year;
    return benchmark;
}

double score_of(const Metrics& m, const std::string& metric) {
    if (metric == "sharpe") {
        return m.sharpe;
    }
    if (metric == "sortino") {
        return m.sortino;
    }
    if (metric == "cagr") {
        return m.cagr;
    }
    if (metric == "total_return") {
        return m.total_return;
    }
    if (metric == "calmar") {
        return m.calmar;
    }
    if (metric == "max_drawdown") {
        return -m.max_drawdown;
    }
    if (metric == "profit_factor") {
        return m.profit_factor;
    }
    if (metric == "win_rate") {
        return m.win_rate;
    }
    throw Error("unknown metric '" + metric +
                    "'; use sharpe, sortino, cagr, total_return, calmar, max_drawdown, profit_factor, or win_rate",
                "sweep.metric");
}

std::size_t thread_count(std::size_t requested, std::size_t work) {
#if defined(__EMSCRIPTEN__)
    static_cast<void>(requested);
    static_cast<void>(work);
    return 1;
#else
    const std::size_t available = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t threads = requested == 0 ? available : requested;
    return std::max<std::size_t>(1, std::min(threads, work));
#endif
}

// Runs body(i) for every i below count on `threads` threads. The calling thread works too, and is the only one to
// report progress. Results must go into slots indexed by i, so they don't depend on which thread ran what.
void parallel_for(std::size_t count,
                  std::size_t threads,
                  const std::function<void(std::size_t)>& body,
                  const Progress& progress) {
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    const auto work = [&](bool reporter) {
        std::size_t reported = 0;
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count) {
                break;
            }
            try {
                body(i);
            } catch (...) {
                const std::lock_guard<std::mutex> lock(failure_mutex);
                if (!failure) {
                    failure = std::current_exception();
                }
                next.store(count);
            }
            const std::size_t finished = done.fetch_add(1) + 1;
            if (reporter && progress) {
                const std::size_t percent = finished * 100 / count;
                if (percent > reported) {
                    reported = percent;
                    progress(static_cast<double>(finished) / static_cast<double>(count));
                }
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads > 0 ? threads - 1 : 0);
    for (std::size_t t = 1; t < threads; ++t) {
        pool.emplace_back(work, false);
    }
    work(true);
    for (std::thread& thread : pool) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

double binomial(std::size_t n, std::size_t k) {
    double result = 1.0;
    for (std::size_t i = 1; i <= k; ++i) {
        result = result * static_cast<double>(n - k + i) / static_cast<double>(i);
    }
    return result;
}

// As many blocks as the budget allows, from 16 down, each at least five bars long.
std::size_t choose_blocks(std::size_t bars, std::size_t trials, std::size_t max_blocks) {
    for (std::size_t blocks = std::min<std::size_t>(max_blocks, 16) & ~std::size_t{1}; blocks >= 4; blocks -= 2) {
        if (bars / blocks < 5) {
            continue;
        }
        if (binomial(blocks, blocks / 2) * static_cast<double>(trials) * static_cast<double>(blocks) <= 1.5e8) {
            return blocks;
        }
    }
    return 0;
}

struct BlockStats {
    std::vector<double> sum;
    std::vector<double> squares;
};

BlockStats block_stats(const std::vector<double>& returns, std::size_t blocks) {
    BlockStats stats{std::vector<double>(blocks, 0.0), std::vector<double>(blocks, 0.0)};
    const std::size_t n = returns.size();
    for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t i = b * n / blocks; i < (b + 1) * n / blocks; ++i) {
            stats.sum[b] += returns[i];
            stats.squares[b] += returns[i] * returns[i];
        }
    }
    return stats;
}

double block_sharpe(double sum, double squares, double count) {
    if (count < 2.0) {
        return 0.0;
    }
    const double mean = sum / count;
    const double variance = (squares - sum * mean) / (count - 1.0);
    return variance > 0.0 ? mean / std::sqrt(variance) : 0.0;
}

OverfitReport cscv(const std::vector<BlockStats>& stats, std::size_t bars) {
    OverfitReport report;
    const std::size_t trials = stats.size();
    report.trials = trials;
    if (trials < 2 || stats[0].sum.empty()) {
        report.pbo = undefined;
        report.oos_loss_probability = undefined;
        return report;
    }
    const std::size_t blocks = stats[0].sum.size();
    report.blocks = blocks;
    std::vector<double> counts(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
        counts[b] = static_cast<double>((b + 1) * bars / blocks - b * bars / blocks);
    }
    std::vector<double> total_sum(trials, 0.0);
    std::vector<double> total_squares(trials, 0.0);
    for (std::size_t n = 0; n < trials; ++n) {
        for (std::size_t b = 0; b < blocks; ++b) {
            total_sum[n] += stats[n].sum[b];
            total_squares[n] += stats[n].squares[b];
        }
    }

    const auto combinations = static_cast<std::size_t>(binomial(blocks, blocks / 2));
    const std::size_t stride = std::max<std::size_t>(1, combinations / 1000);
    std::vector<double> in_sample(trials);
    std::vector<double> out_of_sample(trials);
    std::size_t overfit = 0;
    std::size_t losses = 0;
    // Every way of picking half the blocks as the in-sample set, in increasing order of their bitmask.
    for (std::uint32_t mask = 0; mask < (1U << blocks); ++mask) {
        if (static_cast<std::size_t>(std::popcount(mask)) != blocks / 2) {
            continue;
        }
        double is_count = 0.0;
        for (std::size_t b = 0; b < blocks; ++b) {
            if ((mask >> b) & 1U) {
                is_count += counts[b];
            }
        }
        const double oos_count = static_cast<double>(bars) - is_count;
        std::size_t winner = 0;
        for (std::size_t n = 0; n < trials; ++n) {
            double sum = 0.0;
            double squares = 0.0;
            for (std::size_t b = 0; b < blocks; ++b) {
                if ((mask >> b) & 1U) {
                    sum += stats[n].sum[b];
                    squares += stats[n].squares[b];
                }
            }
            in_sample[n] = block_sharpe(sum, squares, is_count);
            out_of_sample[n] = block_sharpe(total_sum[n] - sum, total_squares[n] - squares, oos_count);
            if (in_sample[n] > in_sample[winner]) {
                winner = n;
            }
        }
        // The winner's rank out of sample, ties shared, as a fraction in (0, 1).
        double below = 0.0;
        double ties = 0.0;
        for (std::size_t n = 0; n < trials; ++n) {
            if (n == winner) {
                continue;
            }
            if (out_of_sample[n] < out_of_sample[winner]) {
                below += 1.0;
            } else if (out_of_sample[n] == out_of_sample[winner]) {
                ties += 1.0;
            }
        }
        const double omega = (below + ties / 2.0 + 1.0) / (static_cast<double>(trials) + 1.0);
        const double logit = math::log(omega / (1.0 - omega));
        overfit += logit <= 0.0 ? 1 : 0;
        losses += out_of_sample[winner] < 0.0 ? 1 : 0;
        // Every split counts toward the probabilities; an even sample of them is kept to draw.
        if (report.splits % stride == 0 && report.degradation.size() < 1000) {
            report.logits.push_back(logit);
            report.degradation.push_back({in_sample[winner], out_of_sample[winner]});
        }
        ++report.splits;
    }
    report.pbo = static_cast<double>(overfit) / static_cast<double>(report.splits);
    report.oos_loss_probability = static_cast<double>(losses) / static_cast<double>(report.splits);
    return report;
}

StrategySpec with_params(const StrategySpec& base, const SweepSpec& spec, double x, double y) {
    StrategySpec strategy = base;
    strategy.params[spec.x.param] = x;
    if (spec.y) {
        strategy.params[spec.y->param] = y;
    }
    return strategy;
}

void check_axis(const StrategySpec& base, const SweepAxis& axis, const std::string& field) {
    if (base.id == "custom") {
        const Rules& rules = base.rules;
        const std::pair<const std::string*, const char*> sources[] = {{&rules.long_entry, "strategy.long_entry"},
                                                                      {&rules.long_exit, "strategy.long_exit"},
                                                                      {&rules.short_entry, "strategy.short_entry"},
                                                                      {&rules.short_exit, "strategy.short_exit"}};
        for (const auto& [source, rule_field] : sources) {
            const auto names = expr::referenced_parameters(*source, rule_field);
            if (std::find(names.begin(), names.end(), axis.param) != names.end()) {
                return;
            }
        }
        throw Error("the rules don't use $" + axis.param + ", so sweeping it would change nothing", field + ".param");
    }
    const StrategyInfo* info = find_strategy(base.id);
    if (info == nullptr) {
        throw Error("unknown strategy '" + base.id + "'", "strategy.id");
    }
    const bool known = std::any_of(info->params.begin(), info->params.end(), [&](const ParamSpec& param) {
        return param.name == axis.param;
    });
    if (!known) {
        throw Error(info->name + " has no parameter '" + axis.param + "'", field + ".param");
    }
}

}  // namespace

BacktestResult run_benchmark(const BarSeries& bars, const BacktestConfig& config, std::size_t begin, std::size_t end) {
    const std::vector<std::int8_t> always(bars.size(), 1);
    return run_backtest(bars, always, benchmark_config(config), begin, end);
}

BacktestReport run_report(const BarSeries& bars,
                          indicators::Cache& cache,
                          const StrategySpec& strategy,
                          const BacktestConfig& config,
                          std::size_t begin,
                          std::size_t end) {
    validate(config);
    BacktestReport report;
    report.signals = generate_signals(strategy, cache, config.allow_short, true);
    report.result = run_backtest(bars, report.signals.direction, config, begin, end);
    report.benchmark = run_benchmark(bars, config, begin, end);
    report.benchmark_metrics = compute_metrics(bars, report.benchmark, {}, config.risk_free_rate);
    const std::vector<double> benchmark_returns = bar_returns(report.benchmark);
    report.metrics = compute_metrics(bars, report.result, benchmark_returns, config.risk_free_rate);
    return report;
}

std::vector<double> SweepAxis::values(const std::string& field) const {
    if (param.empty()) {
        throw Error("name the parameter to sweep", field + ".param");
    }
    for (const auto& [name, value] : {std::pair<const char*, double>{"from", from}, {"to", to}}) {
        if (!std::isfinite(value)) {
            throw Error("the range's ends must be finite numbers", field + "." + name);
        }
    }
    if (!(std::isfinite(step) && step > 0.0)) {
        throw Error("the step must be a positive number", field + ".step");
    }
    if (to < from) {
        throw Error("the range must not end before it starts", field + ".to");
    }
    const double count = std::floor((to - from) / step + 1e-9) + 1.0;
    if (count > static_cast<double>(max_sweep_trials)) {
        throw Error("a sweep axis can have at most " + std::to_string(max_sweep_trials) + " values", field + ".step");
    }
    std::vector<double> out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        // Rounded so that 0.1 steps give 0.3, not 0.30000000000000004.
        out.push_back(std::round((from + static_cast<double>(i) * step) * 1e9) / 1e9);
    }
    return out;
}

double expected_max_sharpe(double sharpe_variance, std::size_t trials) {
    if (trials < 2 || !(sharpe_variance > 0.0)) {
        return 0.0;
    }
    const auto n = static_cast<double>(trials);
    return std::sqrt(sharpe_variance) * ((1.0 - math::euler_gamma) * math::normal_quantile(1.0 - 1.0 / n) +
                                         math::euler_gamma * math::normal_quantile(1.0 - 1.0 / (n * math::e)));
}

OverfitReport probability_of_overfitting(const std::vector<std::vector<double>>& trial_returns,
                                         std::size_t max_blocks) {
    if (trial_returns.empty()) {
        return cscv({}, 0);
    }
    const std::size_t bars = trial_returns[0].size();
    const std::size_t blocks = choose_blocks(bars, trial_returns.size(), max_blocks);
    if (blocks == 0) {
        return cscv({}, bars);
    }
    std::vector<BlockStats> stats;
    stats.reserve(trial_returns.size());
    for (const auto& returns : trial_returns) {
        if (returns.size() != bars) {
            throw Error("internal: every trial needs the same number of returns");
        }
        stats.push_back(block_stats(returns, blocks));
    }
    return cscv(stats, bars);
}

SweepResult run_sweep(const BarSeries& bars,
                      indicators::Cache& cache,
                      const StrategySpec& base,
                      const BacktestConfig& config,
                      std::size_t begin,
                      std::size_t end,
                      const SweepSpec& spec,
                      const Progress& progress) {
    validate(config);
    if (begin >= end || end > bars.size()) {
        throw Error("the sweep needs at least one bar in its date range", "range");
    }
    SweepResult out;
    out.xs = spec.x.values("sweep.x");
    out.ys = spec.y ? spec.y->values("sweep.y") : std::vector<double>{undefined};
    check_axis(base, spec.x, "sweep.x");
    if (spec.y) {
        check_axis(base, *spec.y, "sweep.y");
        if (spec.y->param == spec.x.param) {
            throw Error("sweep two different parameters", "sweep.y.param");
        }
    }
    static_cast<void>(score_of(Metrics{}, spec.metric));
    const std::size_t total = out.xs.size() * out.ys.size();
    if (total > max_sweep_trials) {
        throw Error("a sweep can run at most " + std::to_string(max_sweep_trials) + " combinations, not " +
                        std::to_string(total),
                    "sweep");
    }

    const std::size_t bars_in_range = end - begin;
    const std::size_t blocks = spec.overfitting ? choose_blocks(bars_in_range, total, 16) : 0;
    std::vector<BlockStats> stats(blocks > 0 ? total : 0);
    out.trials.resize(total);
    out.threads = thread_count(spec.threads, total);
    parallel_for(
        total,
        out.threads,
        [&](std::size_t i) {
            Trial& trial = out.trials[i];
            trial.x = out.xs[i % out.xs.size()];
            trial.y = out.ys[i / out.xs.size()];
            try {
                const Signals signals =
                    generate_signals(with_params(base, spec, trial.x, trial.y), cache, config.allow_short, false);
                const BacktestResult result = run_backtest(bars, signals.direction, config, begin, end);
                trial.metrics = compute_metrics(bars, result, {}, config.risk_free_rate);
                trial.score = score_of(trial.metrics, spec.metric);
                trial.valid = true;
                if (blocks > 0) {
                    stats[i] = block_stats(bar_returns(result), blocks);
                }
            } catch (const Error& error) {
                trial.error = error.what();
            }
        },
        progress);

    // Combinations that can't run (a fast window above the slow one) are expected; a sweep where none can run is an
    // error in the request, such as a typo in a rule.
    const auto first_valid = std::find_if(out.trials.begin(), out.trials.end(), [](const Trial& t) { return t.valid; });
    if (first_valid == out.trials.end()) {
        throw Error("no combination could run: " + out.trials.front().error, "sweep");
    }
    for (std::size_t i = 0; i < total; ++i) {
        const Trial& trial = out.trials[i];
        if (trial.valid && !std::isnan(trial.score) && (!out.best || trial.score > out.trials[*out.best].score)) {
            out.best = i;
        }
    }

    if (spec.overfitting) {
        std::vector<BlockStats> valid_stats;
        std::vector<double> sharpes;
        const double annualize = std::sqrt(config.bars_per_year > 0.0 ? config.bars_per_year : bars.bars_per_year());
        for (std::size_t i = 0; i < total; ++i) {
            if (!out.trials[i].valid) {
                continue;
            }
            if (blocks > 0) {
                valid_stats.push_back(stats[i]);
            }
            if (std::isfinite(out.trials[i].metrics.sharpe)) {
                sharpes.push_back(out.trials[i].metrics.sharpe / annualize);
            }
        }
        out.overfit = blocks > 0 ? cscv(valid_stats, bars_in_range) : cscv({}, bars_in_range);
        out.overfit.trials = valid_stats.empty() ? sharpes.size() : valid_stats.size();
        out.overfit.best_sharpe = undefined;
        out.overfit.expected_max_sharpe = undefined;
        out.overfit.deflated_sharpe = undefined;
        if (out.best && sharpes.size() >= 2) {
            const ReturnMoments spread = moments(sharpes);
            const double threshold = expected_max_sharpe(spread.stdev * spread.stdev, sharpes.size());
            const Metrics& best = out.trials[*out.best].metrics;
            out.overfit.best_sharpe = best.sharpe;
            out.overfit.expected_max_sharpe = threshold * annualize;
            out.overfit.deflated_sharpe =
                probabilistic_sharpe(best.sharpe / annualize, threshold, bars_in_range, best.skewness, best.kurtosis);
        }
    }
    return out;
}

WalkForwardResult run_walk_forward(const BarSeries& bars,
                                   indicators::Cache& cache,
                                   const StrategySpec& base,
                                   const BacktestConfig& config,
                                   std::size_t begin,
                                   std::size_t end,
                                   const WalkForwardSpec& spec,
                                   const Progress& progress) {
    validate(config);
    if (!(spec.in_sample_years >= 0.25 && spec.in_sample_years <= 50.0)) {
        throw Error("the in-sample window must be from 0.25 to 50 years", "walkforward.in_sample_years");
    }
    if (!(spec.out_of_sample_years >= 0.05 && spec.out_of_sample_years <= 20.0)) {
        throw Error("the out-of-sample window must be from 0.05 to 20 years", "walkforward.out_of_sample_years");
    }
    const double per_year = config.bars_per_year > 0.0 ? config.bars_per_year : bars.bars_per_year();
    const auto in_sample = static_cast<std::size_t>(std::llround(spec.in_sample_years * per_year));
    const auto out_of_sample =
        static_cast<std::size_t>(std::max(1LL, std::llround(spec.out_of_sample_years * per_year)));
    if (end > bars.size() || begin + in_sample + out_of_sample > end) {
        throw Error("the date range is too short for a " + text::format_number(spec.in_sample_years) +
                        "-year in-sample window followed by a " + text::format_number(spec.out_of_sample_years) +
                        "-year out-of-sample one",
                    "walkforward");
    }

    WalkForwardResult out;
    std::size_t is_begin = begin;
    std::size_t is_end = begin + in_sample;
    while (is_end < end) {
        WalkForwardFold fold;
        fold.is_begin = is_begin;
        fold.is_end = is_end;
        fold.oos_begin = is_end;
        fold.oos_end = std::min(is_end + out_of_sample, end);
        out.folds.push_back(fold);
        is_end += out_of_sample;
        if (!spec.anchored) {
            is_begin += out_of_sample;
        }
    }
    if (out.folds.size() > 200) {
        throw Error("that makes " + std::to_string(out.folds.size()) + " folds; lengthen the out-of-sample window",
                    "walkforward.out_of_sample_years");
    }

    SweepSpec sweep = spec.sweep;
    sweep.overfitting = false;
    double capital = config.initial_capital;
    BacktestResult& stitched = out.stitched;
    stitched.begin = out.folds.front().oos_begin;
    stitched.end = out.folds.back().oos_end;
    stitched.initial_capital = config.initial_capital;
    stitched.bars_per_year = per_year;
    double is_cagr = 0.0;
    double oos_cagr = 0.0;
    const auto folds = static_cast<double>(out.folds.size());
    for (std::size_t f = 0; f < out.folds.size(); ++f) {
        WalkForwardFold& fold = out.folds[f];
        const Progress fold_progress = [&](double done) {
            if (progress) {
                progress((static_cast<double>(f) + done) / folds);
            }
        };
        const SweepResult optimized =
            run_sweep(bars, cache, base, config, fold.is_begin, fold.is_end, sweep, fold_progress);
        if (!optimized.best) {
            throw Error(
                "no parameters could be scored in the in-sample window from " + format_date(bars.time[fold.is_begin]),
                "walkforward");
        }
        const Trial& winner = optimized.trials[*optimized.best];
        fold.x = winner.x;
        fold.y = winner.y;
        fold.in_sample = winner.metrics;

        StrategySpec chosen = with_params(base, sweep, winner.x, winner.y);
        const Signals signals = generate_signals(chosen, cache, config.allow_short, false);
        BacktestConfig fold_config = config;
        fold_config.initial_capital = capital;
        fold_config.close_at_end = true;
        BacktestResult result = run_backtest(bars, signals.direction, fold_config, fold.oos_begin, fold.oos_end);
        fold.out_of_sample = compute_metrics(bars, result, {}, config.risk_free_rate);
        fold.oos_trades = fold.out_of_sample.trades;
        is_cagr += fold.in_sample.cagr / folds;
        oos_cagr += fold.out_of_sample.cagr / folds;

        stitched.equity.insert(stitched.equity.end(), result.equity.begin(), result.equity.end());
        stitched.position.insert(stitched.position.end(), result.position.begin(), result.position.end());
        stitched.exposure.insert(stitched.exposure.end(), result.exposure.begin(), result.exposure.end());
        stitched.fills.insert(stitched.fills.end(), result.fills.begin(), result.fills.end());
        stitched.trades.insert(stitched.trades.end(), result.trades.begin(), result.trades.end());
        stitched.commissions += result.commissions;
        stitched.slippage += result.slippage;
        stitched.borrow += result.borrow;
        stitched.traded_value += result.traded_value;
        stitched.trimmed += result.trimmed;
        stitched.ruined = stitched.ruined || result.ruined;
        capital = result.equity.back();
        if (capital <= 0.0) {
            break;
        }
    }
    out.benchmark = run_benchmark(bars, config, stitched.begin, stitched.end);
    out.benchmark_metrics = compute_metrics(bars, out.benchmark, {}, config.risk_free_rate);
    if (stitched.equity.size() == out.benchmark.equity.size()) {
        out.stitched_metrics = compute_metrics(bars, stitched, bar_returns(out.benchmark), config.risk_free_rate);
    } else {
        stitched.end = stitched.begin + stitched.equity.size();
        out.stitched_metrics = compute_metrics(bars, stitched, {}, config.risk_free_rate);
    }
    out.efficiency = is_cagr > 0.0 ? oos_cagr / is_cagr : undefined;
    return out;
}

MonteCarloResult run_monte_carlo(const BacktestResult& result,
                                 double years,
                                 const MonteCarloSpec& spec,
                                 const Progress& progress) {
    if (spec.paths < 10 || spec.paths > 10'000) {
        throw Error("the number of paths must be from 10 to 10,000", "montecarlo.paths");
    }
    if (spec.block < 1 || spec.block > 1000) {
        throw Error("the block length must be from 1 to 1,000 bars", "montecarlo.block");
    }
    const std::vector<double> returns = bar_returns(result);
    const std::size_t bars = returns.size();
    if (bars < 2) {
        throw Error("the backtest needs at least two bars to resample", "range");
    }
    const double initial = result.initial_capital;
    const double annualize = std::sqrt(result.bars_per_year);

    MonteCarloResult out;
    out.paths = spec.paths;
    out.block = spec.block;
    const std::size_t points = std::min<std::size_t>(bars, 160);
    for (std::size_t k = 0; k < points; ++k) {
        const std::size_t offset = points == 1 ? bars - 1 : k * (bars - 1) / (points - 1);
        if (out.checkpoints.empty() || out.checkpoints.back() != offset) {
            out.checkpoints.push_back(offset);
        }
    }
    const std::size_t checkpoints = out.checkpoints.size();
    std::vector<double> path_values(spec.paths * checkpoints);
    out.total_returns.resize(spec.paths);
    out.cagrs.resize(spec.paths);
    out.max_drawdowns.resize(spec.paths);
    out.sharpes.resize(spec.paths);

    Random random(spec.seed);
    const double restart = 1.0 / static_cast<double>(spec.block);
    std::size_t losses = 0;
    for (std::size_t path = 0; path < spec.paths; ++path) {
        double equity = initial;
        double peak = initial;
        double drawdown = 0.0;
        double sum = 0.0;
        double squares = 0.0;
        std::size_t index = random.below(bars);
        std::size_t next_checkpoint = 0;
        for (std::size_t t = 0; t < bars; ++t) {
            if (t > 0) {
                index = random.uniform() < restart ? random.below(bars) : (index + 1) % bars;
            }
            const double r = returns[index];
            equity *= 1.0 + r;
            sum += r;
            squares += r * r;
            peak = std::max(peak, equity);
            drawdown = std::max(drawdown, peak > 0.0 ? 1.0 - equity / peak : 1.0);
            if (next_checkpoint < checkpoints && out.checkpoints[next_checkpoint] == t) {
                path_values[path * checkpoints + next_checkpoint] = equity;
                ++next_checkpoint;
            }
        }
        const double total = equity / initial - 1.0;
        out.total_returns[path] = total;
        out.cagrs[path] = equity > 0.0 && years > 0.0 ? math::pow(equity / initial, 1.0 / years) - 1.0 : -1.0;
        out.max_drawdowns[path] = drawdown;
        const auto n = static_cast<double>(bars);
        const double mean = sum / n;
        const double variance = (squares - sum * mean) / (n - 1.0);
        out.sharpes[path] = variance > 0.0 ? mean / std::sqrt(variance) * annualize : undefined;
        losses += total < 0.0 ? 1 : 0;
        if (progress && (path + 1) % std::max<std::size_t>(1, spec.paths / 50) == 0) {
            progress(static_cast<double>(path + 1) / static_cast<double>(spec.paths));
        }
    }
    out.loss_probability = static_cast<double>(losses) / static_cast<double>(spec.paths);

    constexpr std::array<double, 5> levels{0.05, 0.25, 0.5, 0.75, 0.95};
    std::vector<double> column(spec.paths);
    for (std::vector<double>& band : out.bands) {
        band.resize(checkpoints);
    }
    for (std::size_t k = 0; k < checkpoints; ++k) {
        for (std::size_t path = 0; path < spec.paths; ++path) {
            column[path] = path_values[path * checkpoints + k];
        }
        std::sort(column.begin(), column.end());
        for (std::size_t q = 0; q < levels.size(); ++q) {
            const double position = levels[q] * static_cast<double>(spec.paths - 1);
            const auto below = static_cast<std::size_t>(position);
            const std::size_t above = std::min(below + 1, spec.paths - 1);
            out.bands[q][k] = column[below] + (column[above] - column[below]) * (position - static_cast<double>(below));
        }
    }
    const auto sort_defined = [](std::vector<double>& values) {
        values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return std::isnan(v); }),
                     values.end());
        std::sort(values.begin(), values.end());
    };
    sort_defined(out.total_returns);
    sort_defined(out.cagrs);
    sort_defined(out.max_drawdowns);
    sort_defined(out.sharpes);
    return out;
}

}  // namespace nanotrade
