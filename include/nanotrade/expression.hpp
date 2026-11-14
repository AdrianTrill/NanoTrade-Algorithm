#ifndef NANOTRADE_EXPRESSION_HPP
#define NANOTRADE_EXPRESSION_HPP

#include "nanotrade/indicators.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The strategy language: expressions over price series, such as
//
//     crossover(ema(close, 12), ema(close, 26)) and rsi(close, 14) < 70
//
// Numbers, the price columns, arithmetic, comparisons, and/or/not, x[n] for x n bars ago, $name for a parameter,
// and indicator functions. Every built-in strategy is written in it, so a sweep over a custom strategy's
// parameters works exactly as one over a built-in's.
namespace nanotrade::expr {

using Params = std::map<std::string, double, std::less<>>;

enum class Type : std::uint8_t {
    Number,
    Condition
};

// An indicator an expression uses, which a chart can draw.
struct Plot {
    std::string key;    // canonical text, which is also its key in the indicator cache: "sma(close,20)"
    std::string label;  // for a legend: "SMA(20)"
    bool price_scale{false};
    bool bounded{false};  // an oscillator between 0 and 100
};

struct Node;

class Program {
public:
    // Throws nanotrade::Error carrying `field`, and the position and length of the problem within `source`.
    static Program compile(std::string_view source, const Params& params, const std::string& field = {});

    Type type() const noexcept;
    const std::string& canonical() const noexcept;  // normalized, with each parameter replaced by its value
    // Numbers, or for a condition 1 where it holds, 0 where it doesn't, and NaN where an input is undefined.
    std::vector<double> evaluate(indicators::Cache& cache) const;
    std::vector<Plot> plots() const;
    // Constants an oscillator is compared with: the 30 of rsi(close, 14) < 30, for a guide line.
    std::vector<double> levels() const;
    // The parameters the source refers to, in order of first use.
    const std::vector<std::string>& parameters() const noexcept;

private:
    std::shared_ptr<const Node> root_;
    std::vector<std::string> parameters_;
};

// The parameters `source` refers to, without compiling it. Throws on text that doesn't even tokenize.
std::vector<std::string> referenced_parameters(std::string_view source, const std::string& field = {});

struct FunctionInfo {
    std::string name;
    std::string signature;  // "sma(series, period)"
    std::string returns;    // "number" or "condition"
    std::string description;
};

const std::vector<FunctionInfo>& function_reference();
const std::vector<std::string>& column_names();

inline constexpr std::size_t max_period = 2000;

}  // namespace nanotrade::expr

#endif
