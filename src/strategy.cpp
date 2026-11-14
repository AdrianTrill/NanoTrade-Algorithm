#include "nanotrade/strategy.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/text.hpp"

#include <algorithm>
#include <cmath>

namespace nanotrade {

const std::vector<StrategyInfo>& strategy_catalog() {
    static const std::vector<StrategyInfo> catalog = {
        {"buy_hold",
         "Buy and hold",
         "Benchmark",
         "Always long: the benchmark every other strategy has to beat.",
         {},
         {},
         {"true", "", "", ""}},
        {"sma_cross",
         "SMA crossover",
         "Trend",
         "Long while the fast simple moving average is above the slow one.",
         {{"fast", "Fast SMA", 20, 2, 200, 1, true}, {"slow", "Slow SMA", 50, 5, 400, 1, true}},
         {{"fast", "slow"}},
         {"sma(close, $fast) > sma(close, $slow)", "", "sma(close, $fast) < sma(close, $slow)", ""}},
        {"ema_cross",
         "EMA crossover",
         "Trend",
         "Long while the fast exponential moving average is above the slow one.",
         {{"fast", "Fast EMA", 12, 2, 200, 1, true}, {"slow", "Slow EMA", 26, 5, 400, 1, true}},
         {{"fast", "slow"}},
         {"ema(close, $fast) > ema(close, $slow)", "", "ema(close, $fast) < ema(close, $slow)", ""}},
        {"macd",
         "MACD",
         "Trend",
         "Long while the MACD line is above its signal line.",
         {{"fast", "Fast EMA", 12, 2, 100, 1, true},
          {"slow", "Slow EMA", 26, 5, 200, 1, true},
          {"signal", "Signal EMA", 9, 2, 50, 1, true}},
         {{"fast", "slow"}},
         {"macd(close, $fast, $slow) > macd_signal(close, $fast, $slow, $signal)",
          "",
          "macd(close, $fast, $slow) < macd_signal(close, $fast, $slow, $signal)",
          ""}},
        {"momentum",
         "Time-series momentum",
         "Trend",
         "Long while the price is above where it was a lookback ago.",
         {{"lookback", "Lookback", 126, 5, 500, 1, true}},
         {},
         {"roc(close, $lookback) > 0", "", "roc(close, $lookback) < 0", ""}},
        {"donchian",
         "Donchian breakout",
         "Breakout",
         "Buy a close above the highest high of the entry window; sell one below the lowest low of the exit window, "
         "as the Turtles did.",
         {{"entry", "Entry window", 55, 5, 250, 1, true}, {"exit", "Exit window", 20, 2, 250, 1, true}},
         {},
         {"close > highest(high, $entry)[1]",
          "close < lowest(low, $exit)[1]",
          "close < lowest(low, $entry)[1]",
          "close > highest(high, $exit)[1]"}},
        {"rsi_reversion",
         "RSI reversion",
         "Mean reversion",
         "Buy when the RSI is oversold and sell when it is overbought.",
         {{"period", "RSI period", 14, 2, 100, 1, true},
          {"lower", "Oversold", 30, 5, 50, 1, false},
          {"upper", "Overbought", 70, 50, 95, 1, false}},
         {{"lower", "upper"}},
         {"rsi(close, $period) < $lower",
          "rsi(close, $period) > $upper",
          "rsi(close, $period) > $upper",
          "rsi(close, $period) < $lower"}},
        {"bollinger",
         "Bollinger reversion",
         "Mean reversion",
         "Buy a close below the lower band and sell when the price is back above the middle.",
         {{"period", "Period", 20, 5, 200, 1, true}, {"k", "Width (k)", 2, 0.5, 4, 0.1, false}},
         {},
         {"close < bb_lower(close, $period, $k)",
          "close > sma(close, $period)",
          "close > bb_upper(close, $period, $k)",
          "close < sma(close, $period)"}},
        {"rsi2_pullback",
         "RSI(2) pullback",
         "Mean reversion",
         "Buy sharp dips in an uptrend: a close above its long SMA with RSI(2) below the dip level, sold once the "
         "close is back above a short SMA (after Connors).",
         {{"trend", "Trend SMA", 200, 20, 400, 1, true},
          {"dip", "RSI(2) dip", 10, 1, 40, 1, false},
          {"exit", "Exit SMA", 5, 2, 50, 1, true}},
         {{"exit", "trend"}},
         {"close > sma(close, $trend) and rsi(close, 2) < $dip",
          "close > sma(close, $exit)",
          "close < sma(close, $trend) and rsi(close, 2) > 100 - $dip",
          "close < sma(close, $exit)"}},
        {"custom",
         "Custom rules",
         "Custom",
         "Your own rules in the strategy language.",
         // The example's parameters. Custom rules may name any others; these are the defaults of the example.
         {{"trend", "Trend SMA", 200, 20, 400, 1, true},
          {"fast", "Fast EMA", 12, 2, 200, 1, true},
          {"slow", "Slow EMA", 26, 5, 400, 1, true}},
         {},
         {"close > sma(close, $trend) and crossover(ema(close, $fast), ema(close, $slow))",
          "crossunder(ema(close, $fast), ema(close, $slow)) or close < sma(close, $trend)",
          "",
          ""}},
    };
    return catalog;
}

const StrategyInfo* find_strategy(std::string_view id) {
    for (const StrategyInfo& info : strategy_catalog()) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

const Rules& rules_of(const StrategySpec& spec) {
    if (spec.id == "custom") {
        return spec.rules;
    }
    const StrategyInfo* info = find_strategy(spec.id);
    if (info == nullptr) {
        throw Error("unknown strategy '" + spec.id + "'", "strategy.id");
    }
    return info->rules;
}

expr::Params resolve_params(const StrategySpec& spec) {
    const StrategyInfo* info = find_strategy(spec.id);
    if (info == nullptr) {
        throw Error("unknown strategy '" + spec.id + "'", "strategy.id");
    }
    if (spec.id == "custom") {
        for (const auto& [name, value] : spec.params) {
            if (!std::isfinite(value)) {
                throw Error("$" + name + " must be a number", "strategy.params." + name);
            }
        }
        return spec.params;
    }

    expr::Params params;
    for (const ParamSpec& param : info->params) {
        params[param.name] = param.default_value;
    }
    for (const auto& [name, value] : spec.params) {
        const auto found = std::find_if(info->params.begin(), info->params.end(), [&](const ParamSpec& param) {
            return param.name == name;
        });
        const std::string field = "strategy.params." + name;
        if (found == info->params.end()) {
            throw Error(info->name + " has no parameter '" + name + "'", field);
        }
        if (!std::isfinite(value)) {
            throw Error(found->label + " must be a number", field);
        }
        if (found->integer && value != std::floor(value)) {
            throw Error(found->label + " must be a whole number, not " + text::format_number(value), field);
        }
        if (value < found->min || value > found->max) {
            throw Error(found->label + " must be from " + text::format_number(found->min) + " to " +
                            text::format_number(found->max) + ", not " + text::format_number(value),
                        field);
        }
        params[name] = value;
    }
    for (const auto& [smaller, larger] : info->less_than) {
        if (!(params[smaller] < params[larger])) {
            const auto label = [&](const std::string& name) {
                return std::find_if(info->params.begin(),
                                    info->params.end(),
                                    [&](const ParamSpec& param) { return param.name == name; })
                    ->label;
            };
            throw Error(label(smaller) + " (" + text::format_number(params[smaller]) + ") must be less than " +
                            label(larger) + " (" + text::format_number(params[larger]) + ")",
                        "strategy.params." + smaller);
        }
    }
    return params;
}

std::vector<std::int8_t> follow_rules(std::size_t bars,
                                      const std::vector<double>& long_entry,
                                      const std::vector<double>& long_exit,
                                      const std::vector<double>& short_entry,
                                      const std::vector<double>& short_exit,
                                      bool allow_short) {
    std::vector<std::int8_t> direction(bars, 0);
    std::int8_t state = 0;
    for (std::size_t t = 0; t < bars; ++t) {
        const bool enter_long = !long_entry.empty() && long_entry[t] == 1.0;
        const bool exit_long = long_exit.empty() ? !enter_long : long_exit[t] == 1.0;
        const bool short_signal = !short_entry.empty() && short_entry[t] == 1.0;
        const bool enter_short = allow_short && short_signal;
        const bool exit_short = short_exit.empty() ? !short_signal : short_exit[t] == 1.0;
        if (state == 0) {
            state = enter_long ? 1 : (enter_short ? -1 : 0);
        } else if (state == 1) {
            if (exit_long) {
                state = enter_short ? -1 : 0;
            }
        } else if (exit_short) {
            state = enter_long ? 1 : 0;
        }
        direction[t] = state;
    }
    return direction;
}

Signals generate_signals(const StrategySpec& spec, indicators::Cache& cache, bool allow_short, bool with_overlays) {
    Signals signals;
    signals.params = resolve_params(spec);
    const Rules& rules = rules_of(spec);
    if (text::trim(rules.long_entry).empty() && text::trim(rules.short_entry).empty()) {
        throw Error("a strategy needs a long entry rule, a short entry rule, or both", "strategy.long_entry");
    }

    struct Rule {
        const std::string* source;
        const char* field;
        bool needed;
    };
    const Rule list[] = {
        {&rules.long_entry, "strategy.long_entry", true},
        {&rules.long_exit, "strategy.long_exit", true},
        {&rules.short_entry, "strategy.short_entry", allow_short},
        {&rules.short_exit, "strategy.short_exit", allow_short},
    };
    std::vector<double> conditions[4];
    std::vector<expr::Program> programs;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::string_view source = text::trim(*list[i].source);
        if (source.empty()) {
            continue;
        }
        // Every rule is compiled, even the short ones of a long-only run, so a typo is reported either way.
        expr::Program program = expr::Program::compile(*list[i].source, signals.params, list[i].field);
        if (program.type() != expr::Type::Condition) {
            throw Error("a rule must be a condition that holds or not, such as close > sma(close, 50)",
                        list[i].field,
                        0,
                        list[i].source->size());
        }
        if (list[i].needed) {
            conditions[i] = program.evaluate(cache);
            programs.push_back(std::move(program));
        }
    }
    signals.direction =
        follow_rules(cache.bars().size(), conditions[0], conditions[1], conditions[2], conditions[3], allow_short);

    if (with_overlays) {
        for (const expr::Program& program : programs) {
            for (const expr::Plot& plot : program.plots()) {
                const bool known = std::any_of(signals.overlays.begin(),
                                               signals.overlays.end(),
                                               [&](const Overlay& overlay) { return overlay.key == plot.key; });
                if (!known && signals.overlays.size() < 8) {
                    const indicators::Series* values = &cache.get(plot.key, [&]() -> indicators::Series {
                        throw Error("internal: " + plot.key + " was not computed");
                    });
                    signals.overlays.push_back({plot.key, plot.label, plot.price_scale, plot.bounded, *values});
                }
            }
            for (const double level : program.levels()) {
                if (std::find(signals.levels.begin(), signals.levels.end(), level) == signals.levels.end()) {
                    signals.levels.push_back(level);
                }
            }
        }
        std::sort(signals.levels.begin(), signals.levels.end());
    }
    return signals;
}

}  // namespace nanotrade
