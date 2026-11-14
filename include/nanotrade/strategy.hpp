#ifndef NANOTRADE_STRATEGY_HPP
#define NANOTRADE_STRATEGY_HPP

#include "nanotrade/expression.hpp"
#include "nanotrade/indicators.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nanotrade {

// A strategy is four rules in the strategy language, read at each bar's close.
struct Rules {
    std::string long_entry;   // go long when this holds
    std::string long_exit;    // leave a long when this holds; empty: when long_entry stops holding
    std::string short_entry;  // go short when this holds and shorts are allowed; empty: never short
    std::string short_exit;   // leave a short when this holds; empty: when short_entry stops holding
};

struct ParamSpec {
    std::string name;
    std::string label;
    double default_value{0.0};
    double min{0.0};
    double max{0.0};
    double step{1.0};
    bool integer{true};
};

struct StrategyInfo {
    std::string id;
    std::string name;
    std::string family;  // Benchmark, Trend, Breakout, Mean reversion, or Custom
    std::string summary;
    std::vector<ParamSpec> params;
    std::vector<std::pair<std::string, std::string>> less_than;  // parameter pairs that must be ordered
    Rules rules;
};

const std::vector<StrategyInfo>& strategy_catalog();
const StrategyInfo* find_strategy(std::string_view id);

struct StrategySpec {
    std::string id{"sma_cross"};  // a catalog id, or "custom"
    expr::Params params;          // values that override the defaults; a custom strategy names its own
    Rules rules;                  // a custom strategy's rules
};

// Fills in defaults and checks every parameter against its range. Throws nanotrade::Error naming the field.
expr::Params resolve_params(const StrategySpec& spec);
const Rules& rules_of(const StrategySpec& spec);

struct Overlay {
    std::string key;
    std::string label;
    bool price_scale{false};
    bool bounded{false};
    std::vector<double> values;
};

struct Signals {
    std::vector<std::int8_t> direction;  // wanted at each bar's close: 1 long, -1 short, 0 flat
    std::vector<Overlay> overlays;       // indicators the rules use, to draw
    std::vector<double> levels;          // guide lines for oscillators, such as RSI 30 and 70
    expr::Params params;                 // the parameters used, with defaults filled in
};

Signals generate_signals(const StrategySpec& spec,
                         indicators::Cache& cache,
                         bool allow_short,
                         bool with_overlays = true);

// The state machine behind every strategy. Conditions are 1, 0, or NaN; an empty one is absent. An exit and an entry
// on the same bar leave the position flat, or reverse it when the opposite entry holds.
std::vector<std::int8_t> follow_rules(std::size_t bars,
                                      const std::vector<double>& long_entry,
                                      const std::vector<double>& long_exit,
                                      const std::vector<double>& short_entry,
                                      const std::vector<double>& short_exit,
                                      bool allow_short);

}  // namespace nanotrade

#endif
