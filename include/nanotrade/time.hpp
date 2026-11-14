#ifndef NANOTRADE_TIME_HPP
#define NANOTRADE_TIME_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nanotrade {

using Timestamp = std::int64_t;  // seconds since 1970-01-01T00:00:00Z

inline constexpr Timestamp seconds_per_day = 86'400;
inline constexpr double days_per_year = 365.2425;

struct CivilDate {
    std::int64_t year{1970};
    unsigned month{1};
    unsigned day{1};
};

// Proleptic Gregorian calendar, after Howard Hinnant's chrono-compatible algorithms.
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept;
CivilDate civil_from_days(std::int64_t days) noexcept;
CivilDate civil_from_timestamp(Timestamp time) noexcept;

// Accepts the forms market data comes in: 2024-01-31 (also with / or . between the parts), 20240131,
// 2024-01-31 09:30, 2024-01-31T09:30:00Z, 2024-01-31T09:30:00+01:00, and Unix time in seconds or milliseconds.
std::optional<Timestamp> parse_timestamp(std::string_view text);

std::string format_date(Timestamp time);       // 2024-01-31
std::string format_timestamp(Timestamp time);  // 2024-01-31 at midnight, 2024-01-31 09:30 otherwise, seconds if any

double years_between(Timestamp from, Timestamp to) noexcept;

}  // namespace nanotrade

#endif
