#include "nanotrade/expression.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/math.hpp"
#include "nanotrade/text.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace nanotrade::expr {

struct Node {
    enum class Kind : std::uint8_t {
        Number,
        Column,
        Call,
        Negate,
        Not,
        Binary,
        Lag
    };

    Kind kind{Kind::Number};
    std::string name;   // the column, function, or operator
    double value{0.0};  // a constant's value, or a lag's number of bars
    std::vector<std::shared_ptr<const Node>> args;
    std::size_t position{0};
    std::size_t length{0};
    Type type{Type::Number};
    bool constant{false};
    bool price_scale{false};
    std::string canonical;
};

namespace {

using NodePtr = std::shared_ptr<const Node>;
using indicators::Series;

constexpr double undefined = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t max_source = 2000;
constexpr std::size_t max_depth = 48;

enum class Arg : std::uint8_t {
    Values,    // a series, such as close or sma(close, 20)
    Period,    // a fixed whole number of bars
    Constant,  // a fixed number
};

struct FunctionSpec {
    const char* name;
    std::vector<Arg> args;
    const char* signature;
    Type result;
    bool inherits_scale;  // on the price scale when its first argument is
    bool bounded;
    bool plotted;
    const char* label;
    const char* description;
};

const std::vector<FunctionSpec>& specs() {
    static const std::vector<FunctionSpec> table = {
        {"sma",
         {Arg::Values, Arg::Period},
         "sma(series, period)",
         Type::Number,
         true,
         false,
         true,
         "SMA",
         "Simple moving average of the last period bars"},
        {"ema",
         {Arg::Values, Arg::Period},
         "ema(series, period)",
         Type::Number,
         true,
         false,
         true,
         "EMA",
         "Exponential moving average, weighting recent bars more, seeded with an SMA"},
        {"rsi",
         {Arg::Values, Arg::Period},
         "rsi(series, period)",
         Type::Number,
         false,
         true,
         true,
         "RSI",
         "Relative strength index with Wilder's smoothing, from 0 to 100"},
        {"stdev",
         {Arg::Values, Arg::Period},
         "stdev(series, period)",
         Type::Number,
         false,
         false,
         true,
         "StDev",
         "Standard deviation of the last period bars"},
        {"highest",
         {Arg::Values, Arg::Period},
         "highest(series, period)",
         Type::Number,
         true,
         false,
         true,
         "Highest",
         "Highest value of the last period bars, this one included"},
        {"lowest",
         {Arg::Values, Arg::Period},
         "lowest(series, period)",
         Type::Number,
         true,
         false,
         true,
         "Lowest",
         "Lowest value of the last period bars, this one included"},
        {"roc",
         {Arg::Values, Arg::Period},
         "roc(series, period)",
         Type::Number,
         false,
         false,
         true,
         "ROC",
         "Rate of change over period bars: 0.05 is a 5% rise"},
        {"change",
         {Arg::Values, Arg::Period},
         "change(series, period)",
         Type::Number,
         false,
         false,
         true,
         "Change",
         "Difference from period bars ago"},
        {"zscore",
         {Arg::Values, Arg::Period},
         "zscore(series, period)",
         Type::Number,
         false,
         false,
         true,
         "Z-score",
         "Distance from the period-bar mean, in standard deviations"},
        {"atr",
         {Arg::Period},
         "atr(period)",
         Type::Number,
         false,
         false,
         true,
         "ATR",
         "Average true range, with Wilder's smoothing"},
        {"macd",
         {Arg::Values, Arg::Period, Arg::Period},
         "macd(series, fast, slow)",
         Type::Number,
         false,
         false,
         true,
         "MACD",
         "MACD line: the fast EMA minus the slow EMA"},
        {"macd_signal",
         {Arg::Values, Arg::Period, Arg::Period, Arg::Period},
         "macd_signal(series, fast, slow, signal)",
         Type::Number,
         false,
         false,
         true,
         "MACD signal",
         "Signal line: an EMA of the MACD line"},
        {"bb_upper",
         {Arg::Values, Arg::Period, Arg::Constant},
         "bb_upper(series, period, k)",
         Type::Number,
         true,
         false,
         true,
         "BB upper",
         "Upper Bollinger band: the SMA plus k standard deviations"},
        {"bb_lower",
         {Arg::Values, Arg::Period, Arg::Constant},
         "bb_lower(series, period, k)",
         Type::Number,
         true,
         false,
         true,
         "BB lower",
         "Lower Bollinger band: the SMA minus k standard deviations"},
        {"crossover",
         {Arg::Values, Arg::Values},
         "crossover(a, b)",
         Type::Condition,
         false,
         false,
         false,
         "",
         "a rises above b on this bar: a > b now and a <= b on the bar before"},
        {"crossunder",
         {Arg::Values, Arg::Values},
         "crossunder(a, b)",
         Type::Condition,
         false,
         false,
         false,
         "",
         "a falls below b on this bar: a < b now and a >= b on the bar before"},
        {"abs", {Arg::Values}, "abs(x)", Type::Number, false, false, false, "", "Absolute value"},
        {"min",
         {Arg::Values, Arg::Values},
         "min(a, b)",
         Type::Number,
         true,
         false,
         false,
         "",
         "The smaller of a and b"},
        {"max", {Arg::Values, Arg::Values}, "max(a, b)", Type::Number, true, false, false, "", "The larger of a and b"},
        {"log", {Arg::Values}, "log(x)", Type::Number, false, false, false, "", "Natural logarithm"},
    };
    return table;
}

const FunctionSpec* find_spec(std::string_view name) {
    for (const FunctionSpec& spec : specs()) {
        if (name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

bool is_column(std::string_view name) {
    const auto& columns = column_names();
    return std::find(columns.begin(), columns.end(), name) != columns.end();
}

std::string suggestion(std::string_view name, const std::vector<std::string>& candidates) {
    const std::string best = text::closest(name, candidates);
    return best.empty() ? std::string() : "; did you mean " + best + "?";
}

struct Token {
    enum class Kind : std::uint8_t {
        Number,
        Name,
        Param,
        Symbol,
        End
    };
    Kind kind{Kind::End};
    std::string text;
    double number{0.0};
    std::size_t position{0};
    std::size_t length{0};
};

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_name_char(char c) {
    return is_name_start(c) || is_digit(c);
}

std::vector<Token> tokenize(std::string_view source, const std::string& field) {
    if (source.size() > max_source) {
        throw Error("expressions are limited to " + std::to_string(max_source) + " characters", field);
    }
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < source.size()) {
        const char c = source[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        const std::size_t start = i;
        if (is_digit(c) || (c == '.' && i + 1 < source.size() && is_digit(source[i + 1]))) {
            while (i < source.size() && (is_digit(source[i]) || source[i] == '.')) {
                ++i;
            }
            if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < source.size() && (source[j] == '+' || source[j] == '-')) {
                    ++j;
                }
                if (j < source.size() && is_digit(source[j])) {
                    i = j;
                    while (i < source.size() && is_digit(source[i])) {
                        ++i;
                    }
                }
            }
            const std::string_view literal = source.substr(start, i - start);
            const auto value = text::parse_number(literal);
            if (!value) {
                throw Error("'" + std::string(literal) + "' is not a number", field, start, i - start);
            }
            tokens.push_back({Token::Kind::Number, std::string(literal), *value, start, i - start});
            continue;
        }
        if (is_name_start(c) || c == '$') {
            const bool param = c == '$';
            if (param) {
                ++i;
            }
            const std::size_t name_start = i;
            while (i < source.size() && is_name_char(source[i])) {
                ++i;
            }
            if (param && i == name_start) {
                throw Error("expected a parameter name after $", field, start, 1);
            }
            tokens.push_back({param ? Token::Kind::Param : Token::Kind::Name,
                              text::lowercase(source.substr(name_start, i - name_start)),
                              0.0,
                              start,
                              i - start});
            continue;
        }
        bool matched = false;
        for (const std::string_view symbol : {"<=", ">=", "==", "!=", "&&", "||"}) {
            if (source.substr(i, 2) == symbol) {
                tokens.push_back({Token::Kind::Symbol, std::string(symbol), 0.0, i, 2});
                i += 2;
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }
        if (std::string_view("()[],+-*/<>!").find(c) != std::string_view::npos) {
            tokens.push_back({Token::Kind::Symbol, std::string(1, c), 0.0, i, 1});
            ++i;
            continue;
        }
        if (c == '=') {
            throw Error("use == to compare for equality", field, i, 1);
        }
        const auto byte = static_cast<unsigned char>(c);
        throw Error(byte < 0x80U ? "unexpected character '" + std::string(1, c) + "'" : "unexpected character",
                    field,
                    i,
                    1);
    }
    tokens.push_back({Token::Kind::End, "", 0.0, source.size(), 0});
    return tokens;
}

std::string number_text(double value) {
    return text::format_number(value);
}

class Parser {
public:
    Parser(std::string_view source, const Params& params, std::string field)
        : tokens_(tokenize(source, field)), params_(params), field_(std::move(field)) {}

    NodePtr parse() {
        if (peek().kind == Token::Kind::End) {
            fail("the expression is empty", peek().position, 0);
        }
        NodePtr node = disjunction(0);
        if (peek().kind != Token::Kind::End) {
            const Token& token = peek();
            if (token.kind == Token::Kind::Symbol && (token.text == ")" || token.text == "]")) {
                fail("unmatched '" + token.text + "'", token.position, token.length);
            }
            fail("expected an operator such as and, or, <, or +, before '" + token.text + "'",
                 token.position,
                 token.length);
        }
        return node;
    }

    const std::vector<std::string>& parameters() const noexcept {
        return used_params_;
    }

private:
    [[noreturn]] void fail(const std::string& message, std::size_t position, std::size_t length) const {
        throw Error(message, field_, position, length);
    }
    [[noreturn]] void fail_at(const Node& node, const std::string& message) const {
        fail(message, node.position, node.length);
    }

    const Token& peek() const {
        return tokens_[index_];
    }
    const Token& advance() {
        return tokens_[index_++];
    }
    bool accept(std::string_view symbol) {
        const Token& token = peek();
        if ((token.kind == Token::Kind::Symbol || token.kind == Token::Kind::Name) && token.text == symbol) {
            ++index_;
            return true;
        }
        return false;
    }
    const Token& expect(std::string_view symbol, const std::string& message) {
        if (!accept(symbol)) {
            const Token& token = peek();
            fail(message, token.position, std::max<std::size_t>(token.length, 1));
        }
        return tokens_[index_ - 1];
    }

    static std::size_t end_of(const Node& node) {
        return node.position + node.length;
    }

    void enter(std::size_t depth, const Token& token) const {
        if (depth > max_depth) {
            fail("the expression is nested too deeply", token.position, token.length);
        }
    }

    NodePtr disjunction(std::size_t depth) {
        NodePtr left = conjunction(depth);
        for (;;) {
            if (accept("or") || accept("||")) {
                left = logical("or", left, conjunction(depth));
            } else {
                return left;
            }
        }
    }

    NodePtr conjunction(std::size_t depth) {
        NodePtr left = negation(depth);
        for (;;) {
            if (accept("and") || accept("&&")) {
                left = logical("and", left, negation(depth));
            } else {
                return left;
            }
        }
    }

    NodePtr negation(std::size_t depth) {
        const Token& token = peek();
        if (accept("not") || accept("!")) {
            enter(depth + 1, token);
            NodePtr operand = negation(depth + 1);
            if (operand->type != Type::Condition) {
                fail_at(*operand, "not applies to a condition, and this is a number");
            }
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Not;
            node->type = Type::Condition;
            node->position = token.position;
            node->length = end_of(*operand) - token.position;
            node->canonical = "(not " + operand->canonical + ")";
            node->args.push_back(std::move(operand));
            return node;
        }
        return comparison(depth);
    }

    static bool is_comparison(const Token& token) {
        if (token.kind != Token::Kind::Symbol) {
            return false;
        }
        return token.text == "<" || token.text == "<=" || token.text == ">" || token.text == ">=" ||
               token.text == "==" || token.text == "!=";
    }

    NodePtr comparison(std::size_t depth) {
        NodePtr left = additive(depth);
        if (!is_comparison(peek())) {
            return left;
        }
        const Token op = advance();
        NodePtr right = additive(depth);
        for (const NodePtr& side : {left, right}) {
            if (side->type != Type::Number) {
                fail_at(*side, "comparisons need numbers on both sides, and this is a condition");
            }
        }
        if (is_comparison(peek())) {
            fail("comparisons don't chain; join them with and", peek().position, peek().length);
        }
        return binary(op.text, std::move(left), std::move(right), Type::Condition);
    }

    NodePtr additive(std::size_t depth) {
        NodePtr left = multiplicative(depth);
        for (;;) {
            const Token& token = peek();
            if (token.kind == Token::Kind::Symbol && (token.text == "+" || token.text == "-")) {
                const std::string op = advance().text;
                left = arithmetic(op, std::move(left), multiplicative(depth));
            } else {
                return left;
            }
        }
    }

    NodePtr multiplicative(std::size_t depth) {
        NodePtr left = unary(depth);
        for (;;) {
            const Token& token = peek();
            if (token.kind == Token::Kind::Symbol && (token.text == "*" || token.text == "/")) {
                const std::string op = advance().text;
                left = arithmetic(op, std::move(left), unary(depth));
            } else {
                return left;
            }
        }
    }

    NodePtr unary(std::size_t depth) {
        const Token& token = peek();
        if (token.kind == Token::Kind::Symbol && token.text == "-") {
            advance();
            enter(depth + 1, token);
            NodePtr operand = unary(depth + 1);
            if (operand->type != Type::Number) {
                fail_at(*operand, "can't negate a condition; use not");
            }
            if (operand->constant) {
                return constant(-operand->value, token.position, end_of(*operand) - token.position);
            }
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Negate;
            node->position = token.position;
            node->length = end_of(*operand) - token.position;
            node->price_scale = false;
            node->canonical = "(-" + operand->canonical + ")";
            node->args.push_back(std::move(operand));
            return node;
        }
        return postfix(depth);
    }

    NodePtr postfix(std::size_t depth) {
        NodePtr node = primary(depth);
        while (peek().kind == Token::Kind::Symbol && peek().text == "[") {
            advance();
            NodePtr count = disjunction(depth + 1);
            const Token& close = expect("]", "expected ']' after the number of bars");
            if (!count->constant || count->type != Type::Number) {
                fail_at(*count, "the number of bars back must be fixed, such as close[1]");
            }
            const double bars = count->value;
            if (bars < 0.0 || bars != std::floor(bars) || bars > static_cast<double>(max_period)) {
                fail_at(*count,
                        "the number of bars back must be a whole number from 0 to " + std::to_string(max_period));
            }
            if (bars == 0.0 || node->constant) {
                continue;
            }
            auto lag = std::make_shared<Node>();
            lag->kind = Node::Kind::Lag;
            lag->type = node->type;
            lag->value = bars;
            lag->price_scale = node->price_scale;
            lag->position = node->position;
            lag->length = close.position + close.length - node->position;
            lag->canonical = node->canonical + "[" + number_text(bars) + "]";
            lag->args.push_back(std::move(node));
            node = std::move(lag);
        }
        return node;
    }

    NodePtr primary(std::size_t depth) {
        const Token token = advance();
        enter(depth, token);
        switch (token.kind) {
            case Token::Kind::Number:
                return constant(token.number, token.position, token.length);
            case Token::Kind::Param: {
                const auto found = params_.find(token.text);
                if (found == params_.end()) {
                    std::vector<std::string> known;
                    for (const auto& [name, value] : params_) {
                        known.push_back(name);
                    }
                    fail("$" + token.text + " is not a parameter" + suggestion(token.text, known),
                         token.position,
                         token.length);
                }
                if (std::find(used_params_.begin(), used_params_.end(), token.text) == used_params_.end()) {
                    used_params_.push_back(token.text);
                }
                return constant(found->second, token.position, token.length);
            }
            case Token::Kind::Name:
                return name(token, depth);
            case Token::Kind::Symbol:
                if (token.text == "(") {
                    NodePtr inner = disjunction(depth + 1);
                    const Token& close = expect(")", "expected ')'");
                    auto node = std::make_shared<Node>(*inner);
                    node->position = token.position;
                    node->length = close.position + close.length - token.position;
                    return node;
                }
                fail("expected a number, a price such as close, or a function such as sma(close, 20)",
                     token.position,
                     token.length);
            case Token::Kind::End:
            default:
                fail("the expression ends too soon", token.position, 0);
        }
    }

    NodePtr name(const Token& token, std::size_t depth) {
        const bool call = peek().kind == Token::Kind::Symbol && peek().text == "(";
        if (!call) {
            if (token.text == "true" || token.text == "false") {
                auto node = std::make_shared<Node>();
                node->kind = Node::Kind::Number;
                node->type = Type::Condition;
                node->constant = true;
                node->value = token.text == "true" ? 1.0 : 0.0;
                node->position = token.position;
                node->length = token.length;
                node->canonical = token.text;
                return node;
            }
            if (is_column(token.text)) {
                auto node = std::make_shared<Node>();
                node->kind = Node::Kind::Column;
                node->name = token.text;
                node->price_scale = token.text != "volume";
                node->position = token.position;
                node->length = token.length;
                node->canonical = token.text;
                return node;
            }
            if (token.text == "and" || token.text == "or" || token.text == "not") {
                fail("'" + token.text + "' needs a condition on each side", token.position, token.length);
            }
            if (find_spec(token.text) != nullptr) {
                fail(token.text + " is a function: " + find_spec(token.text)->signature, token.position, token.length);
            }
            std::vector<std::string> names = column_names();
            for (const FunctionSpec& spec : specs()) {
                names.emplace_back(spec.name);
            }
            fail("unknown name '" + token.text + "'" + suggestion(token.text, names), token.position, token.length);
        }

        const FunctionSpec* spec = find_spec(token.text);
        if (spec == nullptr) {
            if (is_column(token.text)) {
                fail(token.text + " is a price, not a function", token.position, token.length);
            }
            std::vector<std::string> names;
            for (const FunctionSpec& candidate : specs()) {
                names.emplace_back(candidate.name);
            }
            fail("unknown function '" + token.text + "'" + suggestion(token.text, names), token.position, token.length);
        }
        advance();  // (
        std::vector<NodePtr> args;
        if (!accept(")")) {
            for (;;) {
                args.push_back(disjunction(depth + 1));
                if (accept(")")) {
                    break;
                }
                expect(",", "expected ',' or ')' in the arguments of " + std::string(spec->name));
            }
        }
        const Token& close = tokens_[index_ - 1];
        const std::size_t length = close.position + close.length - token.position;
        if (args.size() != spec->args.size()) {
            fail(std::string(spec->name) + " takes " + std::to_string(spec->args.size()) +
                     (spec->args.size() == 1 ? " argument: " : " arguments: ") + spec->signature,
                 token.position,
                 length);
        }

        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Call;
        node->name = spec->name;
        node->type = spec->result;
        node->position = token.position;
        node->length = length;
        node->canonical = std::string(spec->name) + "(";
        for (std::size_t i = 0; i < args.size(); ++i) {
            const Node& arg = *args[i];
            if (arg.type != Type::Number) {
                fail_at(arg, std::string(spec->name) + " needs a number here, not a condition");
            }
            if (spec->args[i] == Arg::Period) {
                if (!arg.constant) {
                    fail_at(arg, "the period must be a fixed number of bars, such as 20 or $fast");
                }
                if (arg.value < 1.0 || arg.value != std::floor(arg.value) ||
                    arg.value > static_cast<double>(max_period)) {
                    fail_at(arg,
                            "the period must be a whole number of bars from 1 to " + std::to_string(max_period) +
                                ", not " + number_text(arg.value));
                }
            } else if (spec->args[i] == Arg::Constant && !arg.constant) {
                fail_at(arg, "this must be a fixed number, such as 2 or $k");
            }
            node->canonical += (i > 0 ? "," : "") + arg.canonical;
        }
        node->canonical += ")";
        node->price_scale = spec->inherits_scale && !args.empty() && args[0]->price_scale;
        node->args = std::move(args);
        return node;
    }

    static NodePtr constant(double value, std::size_t position, std::size_t length) {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Number;
        node->constant = true;
        node->value = value;
        node->position = position;
        node->length = length;
        node->canonical = number_text(value);
        return node;
    }

    NodePtr arithmetic(const std::string& op, NodePtr left, NodePtr right) {
        for (const NodePtr& side : {left, right}) {
            if (side->type != Type::Number) {
                fail_at(*side, "arithmetic needs numbers, and this is a condition");
            }
        }
        if (left->constant && right->constant) {
            double value = 0.0;
            if (op == "+") {
                value = left->value + right->value;
            } else if (op == "-") {
                value = left->value - right->value;
            } else if (op == "*") {
                value = left->value * right->value;
            } else {
                value = right->value == 0.0 ? undefined : left->value / right->value;
            }
            if (!std::isfinite(value)) {
                fail("this works out to a division by zero", left->position, end_of(*right) - left->position);
            }
            return constant(value, left->position, end_of(*right) - left->position);
        }
        const bool scale = op == "+" || op == "-" ? left->price_scale || right->price_scale
                                                  : (left->price_scale && right->constant) ||
                                                        (right->price_scale && left->constant && op == "*");
        NodePtr node = binary(op, std::move(left), std::move(right), Type::Number);
        std::const_pointer_cast<Node>(node)->price_scale = scale;
        return node;
    }

    NodePtr logical(const std::string& op, NodePtr left, NodePtr right) {
        for (const NodePtr& side : {left, right}) {
            if (side->type != Type::Condition) {
                fail_at(*side,
                        "'" + op + "' joins conditions, and this is a number; compare it with something, " +
                            "such as rsi(close, 14) < 30");
            }
        }
        return binary(op, std::move(left), std::move(right), Type::Condition);
    }

    static NodePtr binary(const std::string& op, NodePtr left, NodePtr right, Type type) {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Binary;
        node->name = op;
        node->type = type;
        node->position = left->position;
        node->length = end_of(*right) - left->position;
        const std::string spaced = op == "and" || op == "or" ? " " + op + " " : op;
        node->canonical = "(" + left->canonical + spaced + right->canonical + ")";
        node->args.push_back(std::move(left));
        node->args.push_back(std::move(right));
        return node;
    }

    std::vector<Token> tokens_;
    std::size_t index_{0};
    const Params& params_;
    std::string field_;
    std::vector<std::string> used_params_;
};

std::size_t period_of(const Node& node) {
    return static_cast<std::size_t>(node.value);
}

Series evaluate_node(const Node& node, indicators::Cache& cache);

Series compute_call(const Node& node, indicators::Cache& cache) {
    const std::string& name = node.name;
    const auto& args = node.args;
    const std::size_t size = cache.bars().size();
    const auto argument = [&](std::size_t i) {
        return evaluate_node(*args[i], cache);
    };
    // A component another call shares, through the cache: macd_signal reuses the macd line, the bands the SMA. It is
    // returned by pointer, as it lives in the cache rather than in anything passed here.
    const auto shared = [&](const std::string& key, const std::function<Series()>& make) {
        return &cache.get(key, make);
    };
    const std::string& source = args.empty() ? name : args[0]->canonical;

    if (name == "sma") {
        return indicators::sma(argument(0), period_of(*args[1]));
    }
    if (name == "ema") {
        return indicators::ema(argument(0), period_of(*args[1]));
    }
    if (name == "rsi") {
        return indicators::rsi(argument(0), period_of(*args[1]));
    }
    if (name == "stdev") {
        return indicators::stdev(argument(0), period_of(*args[1]));
    }
    if (name == "highest") {
        return indicators::highest(argument(0), period_of(*args[1]));
    }
    if (name == "lowest") {
        return indicators::lowest(argument(0), period_of(*args[1]));
    }
    if (name == "roc") {
        return indicators::roc(argument(0), period_of(*args[1]));
    }
    if (name == "change") {
        return indicators::change(argument(0), period_of(*args[1]));
    }
    if (name == "zscore") {
        return indicators::zscore(argument(0), period_of(*args[1]));
    }
    if (name == "atr") {
        return indicators::atr(cache.bars(), period_of(*args[0]));
    }
    if (name == "macd" || name == "macd_signal") {
        const std::string fast = number_text(args[1]->value);
        const std::string slow = number_text(args[2]->value);
        const auto line_of = [&] {
            const Series& fast_ema = *shared("ema(" + source + "," + fast + ")",
                                             [&] { return indicators::ema(argument(0), period_of(*args[1])); });
            const Series& slow_ema = *shared("ema(" + source + "," + slow + ")",
                                             [&] { return indicators::ema(argument(0), period_of(*args[2])); });
            Series out(size, undefined);
            for (std::size_t i = 0; i < size; ++i) {
                out[i] = fast_ema[i] - slow_ema[i];
            }
            return out;
        };
        // The macd call is itself cached under the line's key, so only macd_signal may ask the cache for the line:
        // asking from inside its own computation would never finish.
        if (name == "macd") {
            return line_of();
        }
        const Series& line = *shared("macd(" + source + "," + fast + "," + slow + ")", line_of);
        return indicators::ema(line, period_of(*args[3]));
    }
    if (name == "bb_upper" || name == "bb_lower") {
        const std::string period = number_text(args[1]->value);
        const Series& mean = *shared("sma(" + source + "," + period + ")",
                                     [&] { return indicators::sma(argument(0), period_of(*args[1])); });
        const Series& deviation = *shared("stdev(" + source + "," + period + ")",
                                          [&] { return indicators::stdev(argument(0), period_of(*args[1])); });
        const double k = name == "bb_upper" ? args[2]->value : -args[2]->value;
        Series out(size, undefined);
        for (std::size_t i = 0; i < size; ++i) {
            out[i] = mean[i] + k * deviation[i];
        }
        return out;
    }
    if (name == "crossover" || name == "crossunder") {
        const Series a = argument(0);
        const Series b = argument(1);
        const bool up = name == "crossover";
        Series out(size, undefined);
        for (std::size_t i = 1; i < size; ++i) {
            if (indicators::defined(a[i]) && indicators::defined(b[i]) && indicators::defined(a[i - 1]) &&
                indicators::defined(b[i - 1])) {
                const bool crossed = up ? a[i] > b[i] && a[i - 1] <= b[i - 1] : a[i] < b[i] && a[i - 1] >= b[i - 1];
                out[i] = crossed ? 1.0 : 0.0;
            }
        }
        return out;
    }
    Series a = argument(0);
    if (name == "abs") {
        for (double& v : a) {
            v = std::fabs(v);
        }
        return a;
    }
    if (name == "log") {
        for (double& v : a) {
            v = v > 0.0 ? math::log(v) : undefined;
        }
        return a;
    }
    const Series b = argument(1);
    for (std::size_t i = 0; i < size; ++i) {
        if (!indicators::defined(a[i]) || !indicators::defined(b[i])) {
            a[i] = undefined;
        } else {
            a[i] = name == "min" ? std::min(a[i], b[i]) : std::max(a[i], b[i]);
        }
    }
    return a;
}

Series evaluate_node(const Node& node, indicators::Cache& cache) {
    const std::size_t size = cache.bars().size();
    switch (node.kind) {
        case Node::Kind::Number:
            return Series(size, node.value);
        case Node::Kind::Column:
            return cache.column(node.name);
        case Node::Kind::Call:
            return cache.get(node.canonical, [&] { return compute_call(node, cache); });
        case Node::Kind::Negate: {
            Series a = evaluate_node(*node.args[0], cache);
            for (double& v : a) {
                v = -v;
            }
            return a;
        }
        case Node::Kind::Not: {
            Series a = evaluate_node(*node.args[0], cache);
            for (double& v : a) {
                v = indicators::defined(v) ? (v == 0.0 ? 1.0 : 0.0) : undefined;
            }
            return a;
        }
        case Node::Kind::Lag:
            return indicators::lag(evaluate_node(*node.args[0], cache), static_cast<std::size_t>(node.value));
        case Node::Kind::Binary:
            break;
    }

    Series a = evaluate_node(*node.args[0], cache);
    const Series b = evaluate_node(*node.args[1], cache);
    const std::string& op = node.name;
    if (op == "and" || op == "or") {
        // Three-valued logic: false and undefined is false, true or undefined is true; otherwise undefined spreads.
        const bool is_and = op == "and";
        for (std::size_t i = 0; i < size; ++i) {
            const double x = a[i];
            const double y = b[i];
            const double absorbing = is_and ? 0.0 : 1.0;
            if (x == absorbing || y == absorbing) {
                a[i] = absorbing;
            } else if (!indicators::defined(x) || !indicators::defined(y)) {
                a[i] = undefined;
            } else {
                a[i] = 1.0 - absorbing;
            }
        }
        return a;
    }
    for (std::size_t i = 0; i < size; ++i) {
        const double x = a[i];
        const double y = b[i];
        if (!indicators::defined(x) || !indicators::defined(y)) {
            a[i] = undefined;
            continue;
        }
        if (op == "+") {
            a[i] = x + y;
        } else if (op == "-") {
            a[i] = x - y;
        } else if (op == "*") {
            a[i] = x * y;
        } else if (op == "/") {
            a[i] = y == 0.0 ? undefined : x / y;
        } else if (op == "<") {
            a[i] = x < y ? 1.0 : 0.0;
        } else if (op == "<=") {
            a[i] = x <= y ? 1.0 : 0.0;
        } else if (op == ">") {
            a[i] = x > y ? 1.0 : 0.0;
        } else if (op == ">=") {
            a[i] = x >= y ? 1.0 : 0.0;
        } else if (op == "==") {
            a[i] = x == y ? 1.0 : 0.0;
        } else {
            a[i] = x != y ? 1.0 : 0.0;
        }
    }
    return a;
}

std::string label_of(const Node& node) {
    if (node.kind != Node::Kind::Call) {
        return node.canonical;
    }
    const FunctionSpec* spec = find_spec(node.name);
    std::string label = std::string(spec->label) + "(";
    bool first = true;
    for (std::size_t i = 0; i < node.args.size(); ++i) {
        const Node& arg = *node.args[i];
        if (i == 0 && spec->args[0] == Arg::Values && arg.kind == Node::Kind::Column && arg.name == "close") {
            continue;
        }
        label += (first ? "" : ", ") + label_of(arg);
        first = false;
    }
    return label + ")";
}

void collect_plots(const Node& node, std::vector<Plot>& plots) {
    for (const auto& arg : node.args) {
        collect_plots(*arg, plots);
    }
    if (node.kind != Node::Kind::Call) {
        return;
    }
    const FunctionSpec* spec = find_spec(node.name);
    if (!spec->plotted) {
        return;
    }
    const bool known = std::any_of(plots.begin(), plots.end(), [&](const Plot& p) { return p.key == node.canonical; });
    if (!known) {
        plots.push_back({node.canonical, label_of(node), node.price_scale, spec->bounded});
    }
}

void collect_levels(const Node& node, std::vector<double>& levels) {
    for (const auto& arg : node.args) {
        collect_levels(*arg, levels);
    }
    if (node.kind != Node::Kind::Binary || node.type != Type::Condition || node.name == "and" || node.name == "or") {
        return;
    }
    const Node& left = *node.args[0];
    const Node& right = *node.args[1];
    const auto oscillator = [](const Node& side) {
        return side.kind == Node::Kind::Call && !side.price_scale;
    };
    if (left.constant && oscillator(right)) {
        levels.push_back(left.value);
    } else if (right.constant && oscillator(left)) {
        levels.push_back(right.value);
    }
}

}  // namespace

Program Program::compile(std::string_view source, const Params& params, const std::string& field) {
    Parser parser(source, params, field);
    Program program;
    program.root_ = parser.parse();
    program.parameters_ = parser.parameters();
    return program;
}

Type Program::type() const noexcept {
    return root_->type;
}

const std::string& Program::canonical() const noexcept {
    return root_->canonical;
}

std::vector<double> Program::evaluate(indicators::Cache& cache) const {
    return evaluate_node(*root_, cache);
}

std::vector<Plot> Program::plots() const {
    std::vector<Plot> plots;
    collect_plots(*root_, plots);
    return plots;
}

std::vector<double> Program::levels() const {
    std::vector<double> levels;
    collect_levels(*root_, levels);
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    return levels;
}

const std::vector<std::string>& Program::parameters() const noexcept {
    return parameters_;
}

std::vector<std::string> referenced_parameters(std::string_view source, const std::string& field) {
    std::vector<std::string> names;
    for (const Token& token : tokenize(source, field)) {
        if (token.kind == Token::Kind::Param && std::find(names.begin(), names.end(), token.text) == names.end()) {
            names.push_back(token.text);
        }
    }
    return names;
}

const std::vector<FunctionInfo>& function_reference() {
    static const std::vector<FunctionInfo> reference = [] {
        std::vector<FunctionInfo> out;
        for (const FunctionSpec& spec : specs()) {
            out.push_back(
                {spec.name, spec.signature, spec.result == Type::Number ? "number" : "condition", spec.description});
        }
        return out;
    }();
    return reference;
}

const std::vector<std::string>& column_names() {
    static const std::vector<std::string> names = {"open", "high", "low", "close", "volume", "hl2"};
    return names;
}

}  // namespace nanotrade::expr
