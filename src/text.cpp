#include "nanotrade/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <system_error>

namespace nanotrade::text {

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
namespace {

// For a decimal number a double can't hold: whether it is too small rather than too large, which is when its first
// nonzero digit stands for a negative power of ten.
bool too_small(std::string_view body) noexcept {
    const std::size_t e = body.find_first_of("eE");
    const std::string_view mantissa = body.substr(0, e);
    const std::size_t point = std::min(mantissa.find('.'), mantissa.size());
    const std::size_t first = mantissa.find_first_of("123456789");
    if (first == std::string_view::npos) {
        return true;
    }
    long long power = first < point ? static_cast<long long>(point - first) - 1
                                    : static_cast<long long>(point) - static_cast<long long>(first);
    if (e != std::string_view::npos) {
        std::size_t i = e + 1;
        const bool negative = i < body.size() && body[i] == '-';
        if (i < body.size() && (body[i] == '+' || body[i] == '-')) {
            ++i;
        }
        long long exponent = 0;
        for (; i < body.size(); ++i) {
            exponent = std::min(exponent * 10 + (body[i] - '0'), 1'000'000LL);
        }
        power += negative ? -exponent : exponent;
    }
    return power < 0;
}

}  // namespace
#endif

std::optional<double> parse_number(std::string_view text) noexcept {
    std::size_t position = 0;
    bool negative = false;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        negative = text[position] == '-';
        ++position;
    }
    const std::string_view body = text.substr(position);
    // Validate the grammar here, so both parsers below only ever see a plain decimal number.
    bool seen_digit = false;
    bool seen_point = false;
    bool seen_exponent = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c >= '0' && c <= '9') {
            seen_digit = true;
        } else if (c == '.' && !seen_point && !seen_exponent) {
            seen_point = true;
        } else if ((c == 'e' || c == 'E') && seen_digit && !seen_exponent) {
            seen_exponent = true;
            if (i + 1 < body.size() && (body[i + 1] == '+' || body[i + 1] == '-')) {
                ++i;
            }
            if (i + 1 >= body.size()) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if (!seen_digit) {
        return std::nullopt;
    }

    // Both parsers round correctly, and agree at the ends of the range: a number too small for a double is zero, and
    // one too large is an error, whatever the platform.
    double value = 0.0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    const auto [end, error] = std::from_chars(body.data(), body.data() + body.size(), value);
    if (error == std::errc::result_out_of_range) {
        if (!too_small(body)) {
            return std::nullopt;
        }
        value = 0.0;
    } else if (error != std::errc{} || end != body.data() + body.size()) {
        return std::nullopt;
    }
#else
    // Older standard libraries lack floating-point from_chars; their strtod is correctly rounded, and the grammar
    // check above leaves nothing for the locale to change.
    try {
        const std::string copy(body);
        char* end = nullptr;
        value = std::strtod(copy.c_str(), &end);
        if (end != copy.c_str() + copy.size() || std::isinf(value)) {
            return std::nullopt;
        }
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
#endif
    return negative ? -value : value;
}

void append_number(std::string& out, double value, int digits) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    if (value == 0.0) {
        out.push_back('0');
        return;
    }
    // Seventeen digits tell any two doubles apart; beyond them come the exact binary expansion's digits, up to 100,
    // which with the sign, point, and exponent fit the buffer.
    digits = std::min(digits, 100);
    std::array<char, 128> buffer{};
    auto result =
        digits > 0
            ? std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, digits)
            : std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    }
    out.append(buffer.data(), result.ptr);
}

std::string format_number(double value, int digits) {
    std::string out;
    append_number(out, value, digits);
    return out;
}

std::string_view trim(std::string_view text) noexcept {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string closest(std::string_view word, const std::vector<std::string>& candidates) {
    // Edits that only change case are free, so "aapl" finds AAPL and "SMA" finds sma.
    const auto fold = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const auto distance = [&](std::string_view a, std::string_view b) {
        std::vector<std::size_t> row(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) {
            row[j] = j;
        }
        for (std::size_t i = 1; i <= a.size(); ++i) {
            std::size_t diagonal = row[0];
            row[0] = i;
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const std::size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (fold(a[i - 1]) == fold(b[j - 1]) ? 0 : 1)});
                diagonal = above;
            }
        }
        return row[b.size()];
    };
    std::string best;
    std::size_t best_distance = 3;
    for (const std::string& candidate : candidates) {
        const std::size_t d = distance(word, candidate);
        if (d < best_distance) {
            best_distance = d;
            best = candidate;
        }
    }
    return best;
}

}  // namespace nanotrade::text
