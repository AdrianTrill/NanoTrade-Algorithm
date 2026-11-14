// The strategies against a second implementation written here from their definitions: each catalog strategy's
// indicators and state machine again, in plain loops that use neither the engine's indicators nor its strategy
// language, must take the same position on every bar of three stocks and two random walks, long-only and long-short,
// with the default parameters and others; prices in cents are jittered by 1e-9 first, to break their decimal ties.
// Then the parameter checks, the state machine on cases worked by hand, custom rules, the overlays a chart draws, and
// the ties the engine guarantees: the average of equal prices is exactly that price.

#include "check.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/expression.hpp"
#include "nanotrade/indicators.hpp"
#include "nanotrade/random.hpp"
#include "nanotrade/series.hpp"
#include "nanotrade/strategy.hpp"
#include "nanotrade/time.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using nanotrade::BarSeries;
using nanotrade::Error;
using nanotrade::Rules;
using nanotrade::Signals;
using nanotrade::StrategySpec;
using nanotrade::expr::Params;
using Series = std::vector<double>;
using Directions = std::vector<std::int8_t>;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

std::string text_of(const Params& params) {
    std::string out;
    for (const auto& [name, value] : params) {
        out += (out.empty() ? "{" : ", ") + name + " " + text_of(value);
    }
    return out.empty() ? "{}" : out + "}";
}

StrategySpec spec_of(const std::string& id, Params params = {}) {
    StrategySpec spec;
    spec.id = id;
    spec.params = std::move(params);
    return spec;
}

StrategySpec custom_spec(Rules rules, Params params = {}) {
    StrategySpec spec;
    spec.id = "custom";
    spec.rules = std::move(rules);
    spec.params = std::move(params);
    return spec;
}

void expect_directions(const Directions& actual, const Directions& expected, const std::string& message) {
    check::expect_equal(actual.size(), expected.size(), message + ": bars");
    for (std::size_t t = 0; t < expected.size(); ++t) {
        check::expect_equal(static_cast<int>(actual[t]),
                            static_cast<int>(expected[t]),
                            message + ": bar " + std::to_string(t));
    }
}

// Runs `body`, which must throw a nanotrade::Error naming `field`.
template <typename Body>
void expect_error(Body&& body, const std::string& field, const std::string& message) {
    try {
        body();
    } catch (const Error& error) {
        check::expect_equal(error.field(), field, message + ": the field (" + error.what() + ")");
        return;
    } catch (const std::exception& error) {
        throw check::Failure(message + " (threw something else: " + error.what() + ")");
    }
    throw check::Failure(message + " (did not throw)");
}

// ---- The reference indicators, written from their definitions with no attempt at speed ----

// The mean of the n values ending at each bar, added up afresh for every bar.
Series simple_average(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    const double* values = x.data();
    for (std::size_t i = n - 1; i < x.size(); ++i) {
        double sum = 0.0;
        for (std::size_t j = i + 1 - n; j <= i; ++j) {
            sum += values[j];
        }
        out[i] = sum / static_cast<double>(n);
    }
    return out;
}

// Seeded with the mean of the first n values, counting from the first defined one, then e += 2 / (n + 1) * (v - e).
// `x` may be undefined only at its start, as the MACD line is.
Series exponential_average(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    std::size_t start = 0;
    while (start < x.size() && std::isnan(x[start])) {
        ++start;
    }
    if (x.size() - start < n) {
        return out;
    }
    double average = 0.0;
    for (std::size_t j = start; j < start + n; ++j) {
        average += x[j];
    }
    average /= static_cast<double>(n);
    out[start + n - 1] = average;
    const double alpha = 2.0 / static_cast<double>(n + 1);
    for (std::size_t i = start + n; i < x.size(); ++i) {
        average += alpha * (x[i] - average);
        out[i] = average;
    }
    return out;
}

// Wilder's: the first n gains and losses averaged, then each average a = (a * (n - 1) + latest) / n; the RSI is
// 100 - 100 / (1 + gain / loss), 50 where both are 0, and 100 where only the loss is.
Series relative_strength(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    const double count = static_cast<double>(n);
    double gain = 0.0;
    double loss = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        const double delta = x[i] - x[i - 1];
        const double up = delta > 0.0 ? delta : 0.0;
        const double down = delta < 0.0 ? -delta : 0.0;
        if (i <= n) {
            gain += up;
            loss += down;
            if (i < n) {
                continue;
            }
            gain /= count;
            loss /= count;
        } else {
            gain = (gain * (count - 1.0) + up) / count;
            loss = (loss * (count - 1.0) + down) / count;
        }
        if (gain == 0.0 && loss == 0.0) {
            out[i] = 50.0;
        } else if (loss == 0.0) {
            out[i] = 100.0;
        } else {
            out[i] = 100.0 - 100.0 / (1.0 + gain / loss);
        }
    }
    return out;
}

Series rate_of_change(const Series& x, std::size_t n) {
    Series out(x.size(), nan);
    for (std::size_t i = n; i < x.size(); ++i) {
        out[i] = x[i] / x[i - n] - 1.0;
    }
    return out;
}

// The highest, or the lowest, of the n values ending at each bar, that bar included.
Series extreme(const Series& x, std::size_t n, bool take_highest) {
    Series out(x.size(), nan);
    const double* values = x.data();
    for (std::size_t i = n - 1; i < x.size(); ++i) {
        double best = values[i];
        for (std::size_t j = i + 1 - n; j < i; ++j) {
            if (take_highest ? values[j] > best : values[j] < best) {
                best = values[j];
            }
        }
        out[i] = best;
    }
    return out;
}

// x[1]: the value on the bar before.
Series previous(const Series& x) {
    Series out(x.size(), nan);
    for (std::size_t i = 1; i < x.size(); ++i) {
        out[i] = x[i - 1];
    }
    return out;
}

struct Bands {
    Series middle;
    Series lower;
    Series upper;
};

// The SMA, and k population standard deviations either side of it.
Bands bollinger_bands(const Series& x, std::size_t n, double k) {
    Bands bands{simple_average(x, n), Series(x.size(), nan), Series(x.size(), nan)};
    const double* values = x.data();
    for (std::size_t i = n - 1; i < x.size(); ++i) {
        const double mean = bands.middle[i];
        double squares = 0.0;
        for (std::size_t j = i + 1 - n; j <= i; ++j) {
            squares += (values[j] - mean) * (values[j] - mean);
        }
        const double deviation = std::sqrt(squares / static_cast<double>(n));
        bands.lower[i] = mean - k * deviation;
        bands.upper[i] = mean + k * deviation;
    }
    return bands;
}

// ---- The reference strategies ----

struct Comparison {
    std::string text;
    Series left;
    bool greater{true};  // left > right, or else left < right
    Series right;
};

// A rule holds on a bar where every one of its comparisons is defined and true, so an undefined one counts as false.
// A present rule with no comparisons always holds.
struct Rule {
    bool present{false};
    std::vector<Comparison> all;
};

struct ReferenceRules {
    Rule long_entry;
    Rule long_exit;
    Rule short_entry;
    Rule short_exit;
};

Comparison above(const std::string& left_text, Series left, const std::string& right_text, Series right) {
    return {left_text + " > " + right_text, std::move(left), true, std::move(right)};
}

Comparison below(const std::string& left_text, Series left, const std::string& right_text, Series right) {
    return {left_text + " < " + right_text, std::move(left), false, std::move(right)};
}

Rule rule_of(std::vector<Comparison> all) {
    return {true, std::move(all)};
}

bool rule_holds(const Rule& rule, std::size_t t) {
    return rule.present && std::all_of(rule.all.begin(), rule.all.end(), [t](const Comparison& comparison) {
               return comparison.greater ? comparison.left[t] > comparison.right[t]
                                         : comparison.left[t] < comparison.right[t];
           });
}

// Where each rule holds; an absent rule is empty.
struct Holds {
    std::vector<bool> long_entry;
    std::vector<bool> long_exit;
    std::vector<bool> short_entry;
    std::vector<bool> short_exit;
};

std::vector<bool> where_holds(const Rule& rule, std::size_t bars) {
    std::vector<bool> out;
    if (rule.present) {
        out.resize(bars);
        for (std::size_t t = 0; t < bars; ++t) {
            out[t] = rule_holds(rule, t);
        }
    }
    return out;
}

Holds where_holds(const ReferenceRules& rules, std::size_t bars) {
    return {where_holds(rules.long_entry, bars),
            where_holds(rules.long_exit, bars),
            where_holds(rules.short_entry, bars),
            where_holds(rules.short_exit, bars)};
}

// The semantics follow_rules documents: an entry opens a position; a long is left on a bar where its exit holds or,
// without an exit rule, where its entry no longer holds, and a short likewise; the bar that leaves a position takes
// the opposite one if that one's entry holds; and there are shorts only where they're allowed.
Directions reference_directions(std::size_t bars, const Holds& holds, bool allow_short) {
    const auto on = [](const std::vector<bool>& rule, std::size_t t) {
        return !rule.empty() && rule[t];
    };
    Directions out(bars, 0);
    int position = 0;
    for (std::size_t t = 0; t < bars; ++t) {
        const bool go_long = on(holds.long_entry, t);
        const bool go_short = allow_short && on(holds.short_entry, t);
        if (position > 0) {
            const bool leave = holds.long_exit.empty() ? !go_long : on(holds.long_exit, t);
            if (leave) {
                position = go_short ? -1 : 0;
            }
        } else if (position < 0) {
            const bool leave = holds.short_exit.empty() ? !on(holds.short_entry, t) : on(holds.short_exit, t);
            if (leave) {
                position = go_long ? 1 : 0;
            }
        } else if (go_long) {
            position = 1;
        } else if (go_short) {
            position = -1;
        }
        out[t] = static_cast<std::int8_t>(position);
    }
    return out;
}

std::size_t whole(const Params& params, const char* name) {
    return static_cast<std::size_t>(params.at(name));
}

// Long while `a` is above `b` and short while it is below, as the trend strategies are written.
ReferenceRules regime(const std::string& a_text, const Series& a, const std::string& b_text, const Series& b) {
    ReferenceRules rules;
    rules.long_entry = rule_of({above(a_text, a, b_text, b)});
    rules.short_entry = rule_of({below(a_text, a, b_text, b)});
    return rules;
}

struct ReferenceStrategy {
    const char* id;
    std::vector<Params> variants;  // valid parameters to try besides the defaults
    std::function<ReferenceRules(const BarSeries&, const Params&)> rules;
};

const std::vector<ReferenceStrategy>& reference_strategies() {
    static const std::vector<ReferenceStrategy> table = {
        {"buy_hold",
         {},
         [](const BarSeries&, const Params&) {
             ReferenceRules rules;
             rules.long_entry = rule_of({});
             return rules;
         }},
        {"sma_cross",
         {Params{{"fast", 2}, {"slow", 5}}, Params{{"fast", 50}, {"slow", 200}}, Params{{"fast", 10}, {"slow", 400}}},
         [](const BarSeries& bars, const Params& params) {
             return regime("sma(close, fast)",
                           simple_average(bars.close, whole(params, "fast")),
                           "sma(close, slow)",
                           simple_average(bars.close, whole(params, "slow")));
         }},
        {"ema_cross",
         {Params{{"fast", 2}, {"slow", 5}}, Params{{"fast", 50}, {"slow", 200}}, Params{{"fast", 199}, {"slow", 400}}},
         [](const BarSeries& bars, const Params& params) {
             return regime("ema(close, fast)",
                           exponential_average(bars.close, whole(params, "fast")),
                           "ema(close, slow)",
                           exponential_average(bars.close, whole(params, "slow")));
         }},
        {"macd",
         {Params{{"fast", 2}, {"slow", 5}, {"signal", 2}},
          Params{{"fast", 5}, {"slow", 35}, {"signal", 5}},
          Params{{"fast", 100}, {"slow", 200}, {"signal", 50}}},
         [](const BarSeries& bars, const Params& params) {
             const Series fast = exponential_average(bars.close, whole(params, "fast"));
             const Series slow = exponential_average(bars.close, whole(params, "slow"));
             Series line(bars.size());
             for (std::size_t i = 0; i < line.size(); ++i) {
                 line[i] = fast[i] - slow[i];
             }
             const Series signal_line = exponential_average(line, whole(params, "signal"));
             return regime("macd(close, fast, slow)", line, "macd_signal(close, fast, slow, signal)", signal_line);
         }},
        {"momentum",
         {Params{{"lookback", 5}}, Params{{"lookback", 20}}, Params{{"lookback", 500}}},
         [](const BarSeries& bars, const Params& params) {
             return regime("roc(close, lookback)",
                           rate_of_change(bars.close, whole(params, "lookback")),
                           "0",
                           Series(bars.size(), 0.0));
         }},
        {"donchian",
         {Params{{"entry", 5}, {"exit", 2}},
          Params{{"entry", 10}, {"exit", 40}},
          Params{{"entry", 250}, {"exit", 250}}},
         [](const BarSeries& bars, const Params& params) {
             const std::size_t entry_window = whole(params, "entry");
             const std::size_t exit_window = whole(params, "exit");
             ReferenceRules rules;
             rules.long_entry = rule_of({above("close",
                                               bars.close,
                                               "highest(high, entry)[1]",
                                               previous(extreme(bars.high, entry_window, true)))});
             rules.long_exit = rule_of(
                 {below("close", bars.close, "lowest(low, exit)[1]", previous(extreme(bars.low, exit_window, false)))});
             rules.short_entry = rule_of({below("close",
                                                bars.close,
                                                "lowest(low, entry)[1]",
                                                previous(extreme(bars.low, entry_window, false)))});
             rules.short_exit = rule_of({above("close",
                                               bars.close,
                                               "highest(high, exit)[1]",
                                               previous(extreme(bars.high, exit_window, true)))});
             return rules;
         }},
        {"rsi_reversion",
         {Params{{"period", 2}, {"lower", 5}, {"upper", 95}},
          Params{{"period", 7}, {"lower", 45.5}, {"upper", 52.25}},
          Params{{"period", 100}, {"lower", 50}, {"upper", 50.5}}},
         [](const BarSeries& bars, const Params& params) {
             const Series strength = relative_strength(bars.close, whole(params, "period"));
             const Series lower(bars.size(), params.at("lower"));
             const Series upper(bars.size(), params.at("upper"));
             ReferenceRules rules;
             rules.long_entry = rule_of({below("rsi(close, period)", strength, "lower", lower)});
             rules.long_exit = rule_of({above("rsi(close, period)", strength, "upper", upper)});
             rules.short_entry = rule_of({above("rsi(close, period)", strength, "upper", upper)});
             rules.short_exit = rule_of({below("rsi(close, period)", strength, "lower", lower)});
             return rules;
         }},
        {"bollinger",
         {Params{{"period", 5}, {"k", 0.5}}, Params{{"period", 50}, {"k", 1.3}}, Params{{"period", 200}, {"k", 4}}},
         [](const BarSeries& bars, const Params& params) {
             const Bands bands = bollinger_bands(bars.close, whole(params, "period"), params.at("k"));
             ReferenceRules rules;
             rules.long_entry = rule_of({below("close", bars.close, "bb_lower(close, period, k)", bands.lower)});
             rules.long_exit = rule_of({above("close", bars.close, "sma(close, period)", bands.middle)});
             rules.short_entry = rule_of({above("close", bars.close, "bb_upper(close, period, k)", bands.upper)});
             rules.short_exit = rule_of({below("close", bars.close, "sma(close, period)", bands.middle)});
             return rules;
         }},
        {"rsi2_pullback",
         {Params{{"trend", 20}, {"dip", 40}, {"exit", 2}},
          Params{{"trend", 100}, {"dip", 25.5}, {"exit", 10}},
          Params{{"trend", 400}, {"dip", 1}, {"exit", 50}}},
         [](const BarSeries& bars, const Params& params) {
             const Series trend = simple_average(bars.close, whole(params, "trend"));
             const Series strength = relative_strength(bars.close, 2);
             const Series exit_average = simple_average(bars.close, whole(params, "exit"));
             const double dip = params.at("dip");
             ReferenceRules rules;
             rules.long_entry = rule_of({above("close", bars.close, "sma(close, trend)", trend),
                                         below("rsi(close, 2)", strength, "dip", Series(bars.size(), dip))});
             rules.long_exit = rule_of({above("close", bars.close, "sma(close, exit)", exit_average)});
             rules.short_entry =
                 rule_of({below("close", bars.close, "sma(close, trend)", trend),
                          above("rsi(close, 2)", strength, "100 - dip", Series(bars.size(), 100 - dip))});
             rules.short_exit = rule_of({below("close", bars.close, "sma(close, exit)", exit_average)});
             return rules;
         }},
    };
    return table;
}

const ReferenceStrategy* find_reference(const std::string& id) {
    for (const ReferenceStrategy& reference : reference_strategies()) {
        if (reference.id == id) {
            return &reference;
        }
    }
    return nullptr;
}

// The catalog's defaults with `overrides` on top, as resolve_params should fill them in.
Params with_defaults(const std::string& id, const Params& overrides) {
    const nanotrade::StrategyInfo* info = nanotrade::find_strategy(id);
    if (info == nullptr) {
        throw check::Failure("the catalog has no strategy " + id);
    }
    Params params;
    for (const nanotrade::ParamSpec& param : info->params) {
        params[param.name] = param.default_value;
    }
    for (const auto& [name, value] : overrides) {
        params[name] = value;
    }
    return params;
}

// ---- Bars ----

// A geometric random walk of daily bars, its prices in cents, as quotes are, or not.
BarSeries random_walk(std::uint64_t seed, std::size_t bars, double start, double volatility, bool in_cents) {
    nanotrade::Random rng(seed);
    const auto price = [in_cents](double value) {
        return in_cents ? std::round(value * 100.0) / 100.0 : value;
    };
    BarSeries series;
    series.symbol = in_cents ? "CENTS" : "WALK";
    series.reserve(bars);
    const nanotrade::Timestamp first = nanotrade::days_from_civil(2001, 1, 2) * nanotrade::seconds_per_day;
    double last = start;
    for (std::size_t i = 0; i < bars; ++i) {
        const double o = price(last * std::exp(0.3 * volatility * rng.normal()));
        const double c = price(o * std::exp(volatility * rng.normal()));
        const double h = price(std::max(o, c) * (1.0 + volatility * rng.uniform()));
        const double l = price(std::min(o, c) * (1.0 - volatility * rng.uniform()));
        series.push_back(first + static_cast<nanotrade::Timestamp>(i) * nanotrade::seconds_per_day,
                         o,
                         h,
                         l,
                         c,
                         1.0e6 * (1.0 + rng.uniform()));
        last = c;
    }
    return series;
}

// Prices in cents, real or simulated, often make two averages, or an average and a close, tie exactly in decimal, and
// binary floating point then decides the comparison by how each side's rounding falls: two correct summation orders
// can disagree. Scaling each bar's prices by its own factor 1 + 1e-9 u, with u uniform on [-1, 1), breaks every such
// tie by far more than any rounding error, so the engine and the reference can be held to exact agreement. Open, high,
// low, and close share the factor, so the bar stays consistent.
BarSeries jittered(BarSeries bars, std::uint64_t seed) {
    nanotrade::Random rng(seed);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double factor = 1.0 + 1e-9 * (2.0 * rng.uniform() - 1.0);
        bars.open[i] *= factor;
        bars.high[i] *= factor;
        bars.low[i] *= factor;
        bars.close[i] *= factor;
    }
    return bars;
}

struct Dataset {
    Dataset(std::string label, BarSeries series) : name(std::move(label)), bars(std::move(series)), cache(bars) {}

    std::string name;
    BarSeries bars;
    nanotrade::indicators::Cache cache;  // shared by every test, as a parameter sweep shares it
};

const std::vector<std::unique_ptr<Dataset>>& datasets() {
    static const std::vector<std::unique_ptr<Dataset>> all = [] {
        std::vector<std::unique_ptr<Dataset>> out;
        std::uint64_t seed = 101;
        for (const char* symbol : {"AAPL", "GE", "F"}) {
            const std::string path = std::string(NANOTRADE_DATA_DIR) + "/" + symbol + ".csv";
            out.push_back(
                std::make_unique<Dataset>(symbol, jittered(nanotrade::load_csv_file(path, symbol).series, seed++)));
        }
        out.push_back(std::make_unique<Dataset>("a random walk", random_walk(11, 3000, 50.0, 0.02, false)));
        out.push_back(std::make_unique<Dataset>("a random walk in cents",
                                                jittered(random_walk(29, 2500, 20.0, 0.015, true), seed++)));
        return out;
    }();
    return all;
}

// By a std::string_view: GCC 13 takes a reference returned from a call with a temporary std::string to dangle.
Dataset& dataset(std::string_view name) {
    for (const auto& data : datasets()) {
        if (data->name == name) {
            return *data;
        }
    }
    throw check::Failure("no dataset " + std::string(name));
}

// ---- The engine against the reference ----

// Values so close that rounding can decide which is the larger: on decimal prices, almost surely an exact tie.
bool tied(double a, double b) {
    return std::fabs(a - b) <= 1e-12 * std::max(std::fabs(a), std::fabs(b));
}

bool any_tie(const ReferenceRules& rules, std::size_t t) {
    for (const Rule* rule : {&rules.long_entry, &rules.long_exit, &rules.short_entry, &rules.short_exit}) {
        for (const Comparison& comparison : rule->all) {
            if (tied(comparison.left[t], comparison.right[t])) {
                return true;
            }
        }
    }
    return false;
}

std::string describe_bar(const ReferenceRules& rules, std::size_t t) {
    std::ostringstream out;
    out.precision(17);
    const std::pair<const char*, const Rule*> named[] = {{"long entry", &rules.long_entry},
                                                         {"long exit", &rules.long_exit},
                                                         {"short entry", &rules.short_entry},
                                                         {"short exit", &rules.short_exit}};
    for (const auto& [label, rule] : named) {
        if (!rule->present) {
            continue;
        }
        out << "\n      " << label << (rule_holds(*rule, t) ? " holds:" : " doesn't hold:");
        for (const Comparison& comparison : rule->all) {
            const double a = comparison.left[t];
            const double b = comparison.right[t];
            out << " [" << comparison.text << ": " << a << (comparison.greater ? " > " : " < ") << b;
            if (tied(a, b)) {
                out << ", equal to 12 digits";
            }
            out << ']';
        }
    }
    return out.str();
}

std::string engine_indicators(const StrategySpec& spec,
                              nanotrade::indicators::Cache& cache,
                              bool allow_short,
                              std::size_t t) {
    std::ostringstream out;
    out.precision(17);
    out << "\n      the engine's indicators:";
    const Signals signals = nanotrade::generate_signals(spec, cache, allow_short, true);
    for (const nanotrade::Overlay& overlay : signals.overlays) {
        out << ' ' << overlay.key << " = " << overlay.values[t] << ';';
    }
    return out.str();
}

// generate_signals for `id`, with its defaults and each variant, on every dataset, long-only and long-short, must
// match the reference on every bar. A failure tells apart the decisions taken where a rule compares two values equal
// to 12 digits, which rounding decides, from any others.
void expect_agreement(const std::string& id) {
    const ReferenceStrategy* reference = find_reference(id);
    check::expect(reference != nullptr, "no reference for " + id);
    std::vector<Params> variants{Params{}};
    variants.insert(variants.end(), reference->variants.begin(), reference->variants.end());

    std::size_t runs = 0;
    std::size_t long_bars = 0;
    std::size_t short_bars = 0;
    std::size_t untied_decisions = 0;
    std::vector<std::string> disagreements;
    std::string first;
    for (const auto& data : datasets()) {
        const std::size_t bars = data->bars.size();
        for (const Params& overrides : variants) {
            const StrategySpec spec = spec_of(id, overrides);
            const Params params = with_defaults(id, overrides);
            const ReferenceRules rules = reference->rules(data->bars, params);
            const Holds holds = where_holds(rules, bars);
            for (const bool allow_short : {false, true}) {
                ++runs;
                const std::string run =
                    data->name + " " + text_of(params) + (allow_short ? " long-short" : " long-only");
                const Signals signals = nanotrade::generate_signals(spec, data->cache, allow_short, false);
                check::expect(signals.params == params, run + ": the parameters used are " + text_of(signals.params));
                check::expect_equal(signals.direction.size(), bars, run + ": one direction per bar");
                const Directions expected = reference_directions(bars, holds, allow_short);
                std::size_t differ = 0;
                std::size_t decisions = 0;  // bars that differ after one that doesn't
                std::size_t untied = 0;
                std::size_t at = bars;  // the first decision not at a tie, or else the first one
                for (std::size_t t = 0; t < bars; ++t) {
                    if (expected[t] > 0) {
                        ++long_bars;
                    } else if (expected[t] < 0) {
                        ++short_bars;
                    }
                    if (signals.direction[t] == expected[t]) {
                        continue;
                    }
                    ++differ;
                    if (t > 0 && signals.direction[t - 1] != expected[t - 1]) {
                        continue;
                    }
                    ++decisions;
                    const bool tie = any_tie(rules, t);
                    if (!tie) {
                        ++untied;
                    }
                    if (at == bars || (!tie && untied == 1)) {
                        at = t;
                    }
                }
                if (differ == 0) {
                    continue;
                }
                untied_decisions += untied;
                disagreements.push_back(
                    run + ": " + std::to_string(differ) + " bars from " + std::to_string(decisions) + " decisions, " +
                    (untied == 0 ? std::string("all at ties")
                                 : std::to_string(untied) + " not at a tie, from bar " + std::to_string(at)));
                if (first.empty() || (untied > 0 && untied == untied_decisions)) {
                    first = run + ", bar " + std::to_string(at) + " (" + nanotrade::format_date(data->bars.time[at]) +
                            "): the engine says " + std::to_string(static_cast<int>(signals.direction[at])) +
                            ", the reference " + std::to_string(static_cast<int>(expected[at])) + ", from " +
                            std::to_string(at == 0 ? 0 : static_cast<int>(expected[at - 1])) + describe_bar(rules, at) +
                            engine_indicators(spec, data->cache, allow_short, at);
                }
            }
        }
    }
    check::expect(long_bars > 0, id + " is never long, so the comparison shows little");
    check::expect(id == "buy_hold" || short_bars > 0, id + " is never short, so the comparison shows little");
    if (!disagreements.empty()) {
        std::string message = std::to_string(disagreements.size()) + " of " + std::to_string(runs) +
                              " runs disagree with the reference, " +
                              (untied_decisions == 0 ? std::string("every time first where rounding decides a tie:")
                                                     : std::to_string(untied_decisions) + " decisions not at ties:");
        for (const std::string& line : disagreements) {
            message += "\n    " + line;
        }
        throw check::Failure(message + "\n    first: " + first);
    }
}

void every_catalog_strategy_has_a_reference() {
    std::size_t covered = 0;
    for (const nanotrade::StrategyInfo& info : nanotrade::strategy_catalog()) {
        if (info.id != "custom") {
            check::expect(find_reference(info.id) != nullptr, "no reference for " + info.id);
            ++covered;
        }
    }
    check::expect_equal(covered, reference_strategies().size(), "references for strategies the catalog lacks");
    // Every variant is valid.
    for (const ReferenceStrategy& reference : reference_strategies()) {
        for (const Params& variant : reference.variants) {
            check::expect(
                nanotrade::resolve_params(spec_of(reference.id, variant)) == with_defaults(reference.id, variant),
                std::string(reference.id) + " " + text_of(variant));
        }
    }
}

// ---- resolve_params ----

void expect_resolve_error(const std::string& id,
                          const Params& params,
                          const std::string& field,
                          const std::string& message) {
    expect_error([&] { (void)nanotrade::resolve_params(spec_of(id, params)); },
                 field,
                 id + " " + text_of(params) + ": " + message);
}

void resolve_fills_in_defaults() {
    for (const nanotrade::StrategyInfo& info : nanotrade::strategy_catalog()) {
        if (info.id == "custom") {
            continue;
        }
        const Params resolved = nanotrade::resolve_params(spec_of(info.id));
        check::expect_equal(resolved.size(), info.params.size(), info.id + ": one value per parameter");
        for (const nanotrade::ParamSpec& param : info.params) {
            const auto found = resolved.find(param.name);
            check::expect(found != resolved.end(), info.id + ": " + param.name + " is filled in");
            check::expect_equal(found->second, param.default_value, info.id + ": " + param.name + "'s default");
            check::expect(param.min <= param.default_value && param.default_value <= param.max,
                          info.id + ": " + param.name + "'s default is in its range");
        }
    }
    // An override replaces its parameter and leaves the others at their defaults.
    const std::vector<std::pair<std::string, Params>> overrides = {
        {"sma_cross", {{"fast", 10}}},
        {"bollinger", {{"k", 2.5}}},
        {"rsi_reversion", {{"upper", 80}, {"lower", 20.5}}},
        {"macd", {{"signal", 2}, {"slow", 200}, {"fast", 100}}},
        {"sma_cross", {{"fast", 2}, {"slow", 400}}},
        {"bollinger", {{"period", 5}, {"k", 0.5}}},
        {"bollinger", {{"period", 200}, {"k", 4}}},
    };
    for (const auto& [id, params] : overrides) {
        const Params resolved = nanotrade::resolve_params(spec_of(id, params));
        check::expect(resolved == with_defaults(id, params), id + " " + text_of(params) + ": " + text_of(resolved));
    }
    // A custom strategy's parameters are its own.
    const Params own = {{"n", 3}, {"threshold", -0.25}};
    check::expect(nanotrade::resolve_params(custom_spec({"close > sma(close, $n)", "", "", ""}, own)) == own,
                  "a custom strategy's parameters pass through");
}

void resolve_names_the_parameter_at_fault() {
    expect_resolve_error("sma_cross", {{"fastest", 10}}, "strategy.params.fastest", "an unknown parameter");
    expect_resolve_error("buy_hold", {{"fast", 10}}, "strategy.params.fast", "a parameter of a strategy with none");
    expect_resolve_error("sma_cross", {{"fast", 10.5}}, "strategy.params.fast", "a fraction for a whole number");
    expect_resolve_error("donchian", {{"exit", 20.001}}, "strategy.params.exit", "a fraction for a whole number");
    expect_resolve_error("rsi2_pullback", {{"trend", 199.5}}, "strategy.params.trend", "a fraction for a whole number");
    expect_resolve_error("sma_cross", {{"fast", 1}}, "strategy.params.fast", "below the minimum");
    expect_resolve_error("momentum", {{"lookback", -5}}, "strategy.params.lookback", "below the minimum");
    expect_resolve_error("bollinger", {{"k", 0.49}}, "strategy.params.k", "below the minimum");
    expect_resolve_error("sma_cross", {{"slow", 401}}, "strategy.params.slow", "above the maximum");
    expect_resolve_error("bollinger", {{"k", 4.01}}, "strategy.params.k", "above the maximum");
    expect_resolve_error("rsi_reversion", {{"upper", 95.5}}, "strategy.params.upper", "above the maximum");
    expect_resolve_error("macd", {{"signal", 51}}, "strategy.params.signal", "above the maximum");
    expect_resolve_error("sma_cross", {{"fast", nan}}, "strategy.params.fast", "NaN");
    expect_resolve_error("bollinger", {{"k", infinity}}, "strategy.params.k", "infinity");
    expect_resolve_error("no_such_strategy", {}, "strategy.id", "an unknown strategy");
    expect_error([] { (void)nanotrade::resolve_params(custom_spec({"close > $x", "", "", ""}, {{"x", nan}})); },
                 "strategy.params.x",
                 "a custom NaN");
}

void resolve_rejects_misordered_pairs() {
    for (const char* name : {"sma_cross", "ema_cross", "macd"}) {
        const std::string id = name;
        expect_resolve_error(id, {{"fast", 26}, {"slow", 26}}, "strategy.params.fast", "fast == slow");
        expect_resolve_error(id, {{"fast", 30}, {"slow", 26}}, "strategy.params.fast", "fast > slow");
        expect_resolve_error(id, {{"fast", 100}, {"slow", 5}}, "strategy.params.fast", "fast > slow");
        check::expect(nanotrade::resolve_params(spec_of(id, {{"fast", 25}, {"slow", 26}})).at("fast") == 25,
                      id + ": fast just below slow is allowed");
        // Moving only one of them out of order is caught too.
        try {
            (void)nanotrade::resolve_params(spec_of(id, {{"slow", 10}}));
            throw check::Failure(id + ": slow 10 below the default fast was accepted");
        } catch (const Error& error) {
            check::expect(error.field() == "strategy.params.fast" || error.field() == "strategy.params.slow",
                          id + ": slow 10 below the default fast names " + error.field());
        }
    }
    expect_resolve_error("rsi_reversion", {{"lower", 50}, {"upper", 50}}, "strategy.params.lower", "lower == upper");
    check::expect(
        nanotrade::resolve_params(spec_of("rsi_reversion", {{"lower", 49.5}, {"upper", 50}})).at("lower") == 49.5,
        "rsi_reversion: lower just below upper is allowed");
}

// ---- follow_rules, by hand ----

void follow_rules_regime_mode() {
    // Without exit rules a position lasts while its entry holds.
    expect_directions(nanotrade::follow_rules(9, {0, 1, 1, 0, 1, 1, nan, 1, 0}, {}, {}, {}, false),
                      {0, 1, 1, 0, 1, 1, 0, 1, 0},
                      "long-only");
    const Series long_entry = {0, 1, 1, 0, 0, 0, 1, 0, nan, 0};
    const Series short_entry = {0, 0, 0, 1, 1, 0, 0, 1, nan, 1};
    expect_directions(nanotrade::follow_rules(10, long_entry, {}, short_entry, {}, true),
                      {0, 1, 1, -1, -1, 0, 1, -1, 0, -1},
                      "long-short");
    expect_directions(nanotrade::follow_rules(10, long_entry, {}, short_entry, {}, false),
                      {0, 1, 1, 0, 0, 0, 1, 0, 0, 0},
                      "the same rules long-only");
    expect_directions(nanotrade::follow_rules(4, {1, 1, 1, 1}, {}, {}, {}, true), {1, 1, 1, 1}, "always");
    expect_directions(nanotrade::follow_rules(3, {}, {}, {}, {}, true), {0, 0, 0}, "no rules");
    expect_directions(nanotrade::follow_rules(0, {}, {}, {}, {}, true), {}, "no bars");
}

void follow_rules_explicit_exits() {
    // With an exit rule a long lasts until the exit holds, whatever the entry does; an exit and an entry on the same
    // bar leave it flat, and the entry takes it long again on the next bar it holds.
    const Series long_entry = {0, 1, 0, 0, 0, 1, 1, 1, 0};
    const Series long_exit = {1, 0, 0, 1, 0, 0, 1, 0, 0};
    expect_directions(nanotrade::follow_rules(9, long_entry, long_exit, {}, {}, false),
                      {0, 1, 1, 0, 0, 1, 0, 1, 1},
                      "long");
    expect_directions(nanotrade::follow_rules(9, {}, {}, long_entry, long_exit, true),
                      {0, -1, -1, 0, 0, -1, 0, -1, -1},
                      "short");
    // An exit rule on one side only: the long waits for its exit, the short ends when its entry stops holding.
    expect_directions(nanotrade::follow_rules(6, {1, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0}, {0, 0, 0, 1, 1, 0}, {}, true),
                      {1, 1, 0, -1, -1, 0},
                      "an exit rule for the long only");
}

void follow_rules_reversals() {
    // A long's exit and a short's entry on the same bar reverse it, and the other way round; an opposite entry alone
    // doesn't end a position.
    const Series long_entry = {1, 0, 0, 1, 1, 0, 0};
    const Series long_exit = {0, 0, 1, 0, 0, 0, 0};
    const Series short_entry = {0, 0, 1, 0, 0, 1, 0};
    const Series short_exit = {0, 0, 0, 0, 1, 0, 0};
    expect_directions(nanotrade::follow_rules(7, long_entry, long_exit, short_entry, short_exit, true),
                      {1, 1, -1, -1, 1, 1, 1},
                      "long-short");
    expect_directions(nanotrade::follow_rules(7, long_entry, long_exit, short_entry, short_exit, false),
                      {1, 1, 0, 1, 1, 1, 1},
                      "long-only");
    // In regime mode the bar a long's entry stops holding and a short's starts reverses it.
    expect_directions(nanotrade::follow_rules(4, {1, 0, 1, 0}, {}, {0, 1, 0, 1}, {}, true),
                      {1, -1, 1, -1},
                      "regime mode");
}

void follow_rules_without_shorts() {
    const Series short_entry = {1, 1, 0, 1, 1};
    const Series short_exit = {0, 0, 0, 1, 0};
    expect_directions(nanotrade::follow_rules(5, {}, {}, short_entry, {}, false), {0, 0, 0, 0, 0}, "short-only rules");
    expect_directions(nanotrade::follow_rules(5, {}, {}, short_entry, short_exit, false),
                      {0, 0, 0, 0, 0},
                      "short-only rules with an exit");
    expect_directions(nanotrade::follow_rules(5, {}, {}, short_entry, {}, true),
                      {-1, -1, 0, -1, -1},
                      "the same, allowed");
    expect_directions(nanotrade::follow_rules(5, {}, {}, short_entry, short_exit, true),
                      {-1, -1, -1, 0, -1},
                      "the same with an exit, allowed");
    // A long's exit with a short's entry leaves it flat when shorts aren't allowed.
    expect_directions(nanotrade::follow_rules(3, {1, 1, 1}, {0, 1, 0}, {0, 1, 1}, {}, false),
                      {1, 0, 1},
                      "an exit with a short entry, long-only");
}

void follow_rules_nan_conditions() {
    // An undefined condition doesn't hold: it opens nothing, and an undefined exit doesn't close.
    expect_directions(nanotrade::follow_rules(5, {nan, 1, nan, 1, nan}, {1, nan, nan, 1, nan}, {}, {}, false),
                      {0, 1, 1, 0, 0},
                      "long");
    expect_directions(nanotrade::follow_rules(4, {}, {}, {nan, 1, 0, 0}, {nan, nan, nan, 1}, true),
                      {0, -1, -1, 0},
                      "short");
    expect_directions(nanotrade::follow_rules(4, {nan, nan, nan, nan}, {}, {nan, nan, nan, nan}, {}, true),
                      {0, 0, 0, 0},
                      "never defined");
    // In regime mode, an entry that becomes undefined has stopped holding.
    expect_directions(nanotrade::follow_rules(4, {1, nan, 1, 1}, {}, {0, 0, 0, nan}, {}, true),
                      {1, 0, 1, 1},
                      "regime mode");
}

void follow_rules_matches_the_reference() {
    nanotrade::Random rng(5);
    for (int trial = 0; trial < 2000; ++trial) {
        const auto bars = static_cast<std::size_t>(rng.below(30));
        std::array<Series, 4> conditions;
        std::array<std::vector<bool>, 4> holds;
        for (std::size_t r = 0; r < 4; ++r) {
            if (rng.below(4) == 0U) {
                continue;
            }
            conditions[r].resize(bars);
            holds[r].resize(bars);
            for (std::size_t t = 0; t < bars; ++t) {
                const std::uint64_t pick = rng.below(5);
                conditions[r][t] = pick < 2U ? 0.0 : (pick < 4U ? 1.0 : nan);
                holds[r][t] = conditions[r][t] == 1.0;
            }
        }
        for (const bool allow_short : {false, true}) {
            expect_directions(
                nanotrade::follow_rules(bars, conditions[0], conditions[1], conditions[2], conditions[3], allow_short),
                reference_directions(bars, {holds[0], holds[1], holds[2], holds[3]}, allow_short),
                "trial " + std::to_string(trial) + (allow_short ? " long-short" : " long-only"));
        }
    }
}

// ---- Custom strategies ----

void custom_rules_with_params() {
    // The catalog's own rules, as a custom strategy with the same parameters, take the same positions.
    for (const char* name : {"a random walk", "GE"}) {
        Dataset& data = dataset(name);
        for (const ReferenceStrategy& reference : reference_strategies()) {
            const nanotrade::StrategyInfo* info = nanotrade::find_strategy(reference.id);
            check::expect(info != nullptr, std::string("no strategy ") + reference.id);
            const Params params =
                with_defaults(reference.id, reference.variants.empty() ? Params{} : reference.variants.back());
            for (const bool allow_short : {false, true}) {
                const Signals custom =
                    nanotrade::generate_signals(custom_spec(info->rules, params), data.cache, allow_short, false);
                const Signals catalog =
                    nanotrade::generate_signals(spec_of(reference.id, params), data.cache, allow_short, false);
                check::expect(custom.params == params, info->id + ": the custom parameters used");
                expect_directions(custom.direction,
                                  catalog.direction,
                                  data.name + ": " + info->id + "'s rules as a custom strategy");
            }
        }
    }

    // Rules of one's own, with parameters of one's own names, against the reference.
    for (const auto& data : datasets()) {
        const std::size_t bars = data->bars.size();
        const Params params = {{"len", 9}, {"low", 35}, {"high", 60.5}};
        const Series strength = relative_strength(data->bars.close, 9);
        ReferenceRules rules;
        rules.long_entry = rule_of({below("rsi(close, len)", strength, "low", Series(bars, 35.0))});
        rules.long_exit = rule_of({above("rsi(close, len)", strength, "high", Series(bars, 60.5))});
        const StrategySpec spec = custom_spec({"rsi(close, $len) < $low", "rsi(close, $len) > $high", "", ""}, params);
        for (const bool allow_short : {false, true}) {
            expect_directions(nanotrade::generate_signals(spec, data->cache, allow_short, false).direction,
                              reference_directions(bars, where_holds(rules, bars), allow_short),
                              data->name + ": an RSI rule of one's own");
        }

        // Only a short rule: flat unless shorts are allowed.
        ReferenceRules shorts;
        shorts.short_entry =
            rule_of({below("close", data->bars.close, "sma(close, n)", simple_average(data->bars.close, 30))});
        shorts.short_exit = rule_of(
            {above("close", data->bars.close, "highest(high, m)[1]", previous(extreme(data->bars.high, 10, true)))});
        const StrategySpec short_spec =
            custom_spec({"", "", "close < sma(close, $n)", "close > highest(high, $m)[1]"}, {{"n", 30}, {"m", 10}});
        expect_directions(nanotrade::generate_signals(short_spec, data->cache, false, false).direction,
                          Directions(bars, 0),
                          data->name + ": short rules alone, long-only");
        const Directions expected = reference_directions(bars, where_holds(shorts, bars), true);
        check::expect(std::count(expected.begin(), expected.end(), std::int8_t{-1}) > 0, data->name + ": some shorts");
        expect_directions(nanotrade::generate_signals(short_spec, data->cache, true, false).direction,
                          expected,
                          data->name + ": short rules alone, long-short");
    }
}

void custom_rules_rejected() {
    Dataset& data = dataset("a random walk");
    const auto run = [&data](const Rules& rules, const Params& params, bool allow_short) {
        return [&data, rules, params, allow_short] {
            (void)nanotrade::generate_signals(custom_spec(rules, params), data.cache, allow_short);
        };
    };
    for (const bool allow_short : {false, true}) {
        // A parameter the strategy doesn't have.
        expect_error(run({"close > sma(close, $missing)", "", "", ""}, {{"n", 20}}, allow_short),
                     "strategy.long_entry",
                     "an unknown $param");
        expect_error(run({"close > sma(close, $n)", "close < sma(close, $m)", "", ""}, {{"n", 20}}, allow_short),
                     "strategy.long_exit",
                     "an unknown $param in the exit");
        // A number where a condition belongs.
        expect_error(run({"sma(close, 20)", "", "", ""}, {}, allow_short), "strategy.long_entry", "a number");
        expect_error(run({"close > open", "close - open", "", ""}, {}, allow_short),
                     "strategy.long_exit",
                     "a number for the exit");
        expect_error(run({"$n", "", "", ""}, {{"n", 1}}, allow_short), "strategy.long_entry", "a parameter alone");
        // No entry rule at all.
        expect_error(run({"", "close > open", "", ""}, {}, allow_short), "strategy.long_entry", "no entry rule");
        expect_error(run({" \t", "close > open", "  ", "close < open"}, {}, allow_short),
                     "strategy.long_entry",
                     "blank entry rules");
        // A parameter that isn't a number.
        expect_error(run({"close > $x", "", "", ""}, {{"x", infinity}}, allow_short),
                     "strategy.params.x",
                     "an infinite parameter");
    }
    expect_error(run({"close > open", "", "close", ""}, {}, true), "strategy.short_entry", "a number for a short");
}

// ---- Overlays ----

void expect_overlays_fit(const Signals& signals, std::size_t bars, const std::string& message) {
    check::expect(signals.overlays.size() <= 8U, message + ": at most 8 overlays");
    for (std::size_t i = 0; i < signals.overlays.size(); ++i) {
        const nanotrade::Overlay& overlay = signals.overlays[i];
        check::expect_equal(overlay.values.size(), bars, message + ": " + overlay.key + " has one value per bar");
        for (std::size_t j = 0; j < i; ++j) {
            check::expect(signals.overlays[j].key != overlay.key, message + ": " + overlay.key + " twice");
        }
    }
}

const nanotrade::Overlay& overlay_named(const Signals& signals, std::string_view key) {
    for (const nanotrade::Overlay& overlay : signals.overlays) {
        if (overlay.key == key) {
            return overlay;
        }
    }
    std::string keys;
    for (const nanotrade::Overlay& overlay : signals.overlays) {
        keys += " " + overlay.key;
    }
    throw check::Failure("no overlay " + std::string(key) + " among" + keys);
}

void expect_series_near(const Series& actual, const Series& expected, const std::string& message) {
    check::expect_equal(actual.size(), expected.size(), message + ": bars");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check::expect_equal(std::isnan(actual[i]),
                            std::isnan(expected[i]),
                            message + ": defined at bar " + std::to_string(i));
        if (!std::isnan(expected[i])) {
            check::expect_near(actual[i],
                               expected[i],
                               1e-9 * std::fabs(expected[i]),
                               message + ": bar " + std::to_string(i));
        }
    }
}

void overlays_at_most_eight() {
    Dataset& data = dataset("a random walk");
    const StrategySpec spec =
        custom_spec({"sma(close, 5) > sma(close, 6) and sma(close, 7) > sma(close, 8) and "
                     "ema(close, 5) > ema(close, 6) and ema(close, 7) > ema(close, 8) and "
                     "roc(close, 3) > roc(close, 4)",
                     "rsi(close, 14) > 70",
                     "",
                     ""});
    for (const bool allow_short : {false, true}) {
        const Signals signals = nanotrade::generate_signals(spec, data.cache, allow_short);
        check::expect_equal(signals.overlays.size(), std::size_t{8}, "eleven indicators");
        expect_overlays_fit(signals, data.bars.size(), "eleven indicators");
    }
    // Every catalog strategy's overlays fit, long-only and long-short.
    for (const nanotrade::StrategyInfo& info : nanotrade::strategy_catalog()) {
        for (const bool allow_short : {false, true}) {
            const StrategySpec catalog_spec =
                info.id == "custom" ? custom_spec(info.rules, {{"trend", 100}, {"fast", 10}, {"slow", 30}})
                                    : spec_of(info.id);
            expect_overlays_fit(nanotrade::generate_signals(catalog_spec, data.cache, allow_short),
                                data.bars.size(),
                                info.id);
        }
    }
}

void overlays_scale_and_bounds() {
    Dataset& data = dataset("GE");
    const Signals sma = nanotrade::generate_signals(spec_of("sma_cross"), data.cache, true);
    check::expect_equal(sma.overlays.size(), std::size_t{2}, "sma_cross draws each SMA once");
    expect_overlays_fit(sma, data.bars.size(), "sma_cross");
    for (const auto& [key, period] :
         {std::pair<const char*, std::size_t>{"sma(close,20)", 20}, {"sma(close,50)", 50}}) {
        const nanotrade::Overlay& overlay = overlay_named(sma, key);
        check::expect(overlay.price_scale, overlay.key + " is on the price scale");
        check::expect(!overlay.bounded, overlay.key + " isn't bounded");
        expect_series_near(overlay.values, simple_average(data.bars.close, period), overlay.key);
    }
    check::expect(sma.levels.empty(), "an SMA has no guide lines");

    const Signals rsi = nanotrade::generate_signals(spec_of("rsi_reversion"), data.cache, false);
    check::expect_equal(rsi.overlays.size(), std::size_t{1}, "rsi_reversion draws its RSI once");
    expect_overlays_fit(rsi, data.bars.size(), "rsi_reversion");
    const nanotrade::Overlay& strength = overlay_named(rsi, "rsi(close,14)");
    check::expect(!strength.price_scale, "an RSI isn't on the price scale");
    check::expect(strength.bounded, "an RSI is bounded");
    expect_series_near(strength.values, relative_strength(data.bars.close, 14), "rsi(close,14)");
    for (const double value : strength.values) {
        check::expect(std::isnan(value) || (value >= 0.0 && value <= 100.0), "an RSI of " + text_of(value));
    }

    const Signals pullback = nanotrade::generate_signals(spec_of("rsi2_pullback"), data.cache, true);
    expect_overlays_fit(pullback, data.bars.size(), "rsi2_pullback");
    check::expect(overlay_named(pullback, "sma(close,200)").price_scale, "the trend SMA is on the price scale");
    check::expect(overlay_named(pullback, "sma(close,5)").price_scale, "the exit SMA is on the price scale");
    check::expect(!overlay_named(pullback, "rsi(close,2)").price_scale, "RSI(2) isn't on the price scale");
    check::expect(overlay_named(pullback, "rsi(close,2)").bounded, "RSI(2) is bounded");
}

void rsi_reversion_levels() {
    Dataset& data = dataset("F");
    const std::vector<std::pair<Params, std::vector<double>>> cases = {
        {{}, {30, 70}},
        {{{"lower", 25.5}, {"upper", 80}}, {25.5, 80}},
        {{{"period", 2}, {"lower", 50}, {"upper", 50.5}}, {50, 50.5}},
    };
    for (const auto& [params, levels] : cases) {
        for (const bool allow_short : {false, true}) {
            const Signals signals =
                nanotrade::generate_signals(spec_of("rsi_reversion", params), data.cache, allow_short);
            std::string got;
            for (const double level : signals.levels) {
                got += " " + text_of(level);
            }
            check::expect(signals.levels == levels, "rsi_reversion " + text_of(params) + ": levels" + got);
        }
    }
}

void no_overlays_unless_asked() {
    Dataset& data = dataset("AAPL");
    for (const nanotrade::StrategyInfo& info : nanotrade::strategy_catalog()) {
        if (info.id == "custom") {
            continue;
        }
        const Signals signals = nanotrade::generate_signals(spec_of(info.id), data.cache, true, false);
        check::expect(signals.overlays.empty(), info.id + ": no overlays");
        check::expect(signals.levels.empty(), info.id + ": no levels");
        check::expect_equal(signals.direction.size(), data.bars.size(), info.id + ": one direction per bar");
    }
}

// ---- Ties the engine guarantees ----

const double awkward_values[] = {0.1, 0.6, 2.9068, 1e-7, 12345.678};
const std::size_t flat_periods[] = {1, 2, 3, 5, 20, 200};

BarSeries bars_of(const Series& closes) {
    BarSeries bars;
    bars.symbol = "TIES";
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double c = closes[i];
        bars.push_back(static_cast<nanotrade::Timestamp>(i + 1) * nanotrade::seconds_per_day, c, c, c, c, 1.0);
    }
    return bars;
}

// `prefix` values of every size, spikes among them, for a running sum to carry rounding error from, then `run` copies
// of `value`.
Series prefix_then_flat(double value, std::size_t prefix, std::size_t run) {
    nanotrade::Random rng(7);
    Series x;
    for (std::size_t i = 0; i < prefix; ++i) {
        x.push_back(value * std::exp(3.0 * rng.normal()) + (i % 37 == 0 ? 1e6 * value : 0.0));
    }
    x.insert(x.end(), run, value);
    return x;
}

void a_close_on_its_flat_average_is_no_signal() {
    const Series closes = {0.1, 0.6, 0.6};
    check::expect_equal(nanotrade::indicators::sma(closes, 2)[2], 0.6, "sma({0.1, 0.6, 0.6}, 2) on the last bar");
    const BarSeries bars = bars_of(closes);
    nanotrade::indicators::Cache cache(bars);
    for (const bool allow_short : {false, true}) {
        expect_directions(
            nanotrade::generate_signals(custom_spec({"close > sma(close, 2)", "", "", ""}), cache, allow_short)
                .direction,
            {0, 1, 0},
            allow_short ? "long-short" : "long-only");
    }
}

void sma_of_a_flat_window_is_exact() {
    for (const double value : awkward_values) {
        for (const std::size_t period : flat_periods) {
            // From the first bar, after values of every size, and after those and a gap.
            for (const auto& [prefix, gap] : {std::pair<std::size_t, bool>{0, false}, {311, false}, {311, true}}) {
                Series x = prefix_then_flat(value, prefix, 2 * period + 7);
                if (gap) {
                    x[prefix - 1] = nan;
                }
                const Series average = nanotrade::indicators::sma(x, period);
                for (std::size_t t = prefix + period - 1; t < x.size(); ++t) {
                    check::expect_equal(average[t],
                                        value,
                                        "sma over " + std::to_string(period) + " copies of " + text_of(value) +
                                            " after " + std::to_string(prefix) + " other values" +
                                            (gap ? " and a gap" : "") + ", bar " + std::to_string(t));
                }
            }
        }
    }
}

void flat_windows_tie_in_rules() {
    const std::size_t prefix = 311;
    for (const double value : awkward_values) {
        for (const std::size_t period : flat_periods) {
            const BarSeries bars = bars_of(prefix_then_flat(value, prefix, 2 * period + 7));
            nanotrade::indicators::Cache cache(bars);
            const Params params = {{"n", static_cast<double>(period)}};
            const std::array<StrategySpec, 2> specs = {
                custom_spec({"close > sma(close, $n)", "", "close < sma(close, $n)", ""}, params),
                custom_spec({"close < bb_lower(close, $n, 2)", "", "close > bb_upper(close, $n, 2)", ""}, params)};
            for (const StrategySpec& spec : specs) {
                const Directions direction = nanotrade::generate_signals(spec, cache, true, false).direction;
                for (std::size_t t = prefix + period - 1; t < bars.size(); ++t) {
                    check::expect_equal(static_cast<int>(direction[t]),
                                        0,
                                        spec.rules.long_entry + " with n " + std::to_string(period) + " on " +
                                            text_of(value) + ", bar " + std::to_string(t));
                }
            }
        }
    }
}

}  // namespace

int main() {
    return check::run({
        {"every catalog strategy but custom has a reference", every_catalog_strategy_has_a_reference},
        {"buy_hold agrees with the reference", [] { expect_agreement("buy_hold"); }},
        {"sma_cross agrees with the reference", [] { expect_agreement("sma_cross"); }},
        {"ema_cross agrees with the reference", [] { expect_agreement("ema_cross"); }},
        {"macd agrees with the reference", [] { expect_agreement("macd"); }},
        {"momentum agrees with the reference", [] { expect_agreement("momentum"); }},
        {"donchian agrees with the reference", [] { expect_agreement("donchian"); }},
        {"rsi_reversion agrees with the reference", [] { expect_agreement("rsi_reversion"); }},
        {"bollinger agrees with the reference", [] { expect_agreement("bollinger"); }},
        {"rsi2_pullback agrees with the reference", [] { expect_agreement("rsi2_pullback"); }},
        {"resolve_params fills in the defaults", resolve_fills_in_defaults},
        {"resolve_params names the parameter at fault", resolve_names_the_parameter_at_fault},
        {"resolve_params rejects fast >= slow and lower >= upper", resolve_rejects_misordered_pairs},
        {"follow_rules: regime mode without exit rules", follow_rules_regime_mode},
        {"follow_rules: explicit exits", follow_rules_explicit_exits},
        {"follow_rules: an exit and the opposite entry reverse", follow_rules_reversals},
        {"follow_rules: shorts are ignored unless allowed", follow_rules_without_shorts},
        {"follow_rules: NaN conditions don't hold", follow_rules_nan_conditions},
        {"follow_rules matches the reference on random conditions", follow_rules_matches_the_reference},
        {"custom rules with $params", custom_rules_with_params},
        {"custom rules that can't run are rejected", custom_rules_rejected},
        {"at most 8 overlays, one value per bar", overlays_at_most_eight},
        {"overlays: SMA on the price scale, RSI bounded", overlays_scale_and_bounds},
        {"rsi_reversion's levels are {lower, upper}", rsi_reversion_levels},
        {"no overlays unless asked for", no_overlays_unless_asked},
        {"close > sma(close, 2) on closes 0.1, 0.6, 0.6 holds on bar 1 only", a_close_on_its_flat_average_is_no_signal},
        {"sma of a flat window is exactly its value", sma_of_a_flat_window_is_exact},
        {"on a flat window the close is neither above nor below its sma or bands", flat_windows_tie_in_rules},
    });
}
