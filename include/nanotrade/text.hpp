#ifndef NANOTRADE_TEXT_HPP
#define NANOTRADE_TEXT_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nanotrade::text {

// A decimal number with an optional sign, fraction, and exponent; nothing else, so no spaces, hex, inf, or nan.
// Correctly rounded, independently of the C locale.
std::optional<double> parse_number(std::string_view text) noexcept;

// The shortest text that reads back as `value`, or `digits` significant digits when digits > 0. Exact on every
// platform. NaN and the infinities, which JSON can't carry, become null; -0 becomes 0.
void append_number(std::string& out, double value, int digits = 0);
std::string format_number(double value, int digits = 0);

std::string_view trim(std::string_view text) noexcept;
std::string lowercase(std::string_view text);

// The candidate within two edits of `word`, for "did you mean" hints; empty when none is that close.
std::string closest(std::string_view word, const std::vector<std::string>& candidates);

}  // namespace nanotrade::text

#endif
