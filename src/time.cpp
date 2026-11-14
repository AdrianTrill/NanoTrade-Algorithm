#include "nanotrade/time.hpp"

#include <array>
#include <cstddef>

namespace nanotrade {
namespace {

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// Reads exactly `count` digits at `position`, advancing it.
std::optional<std::int64_t> digits(std::string_view text, std::size_t& position, std::size_t count) {
    if (position + count > text.size()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[position + i];
        if (!is_digit(c)) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    position += count;
    return value;
}

bool valid_date(std::int64_t year, std::int64_t month, std::int64_t day) noexcept {
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    constexpr std::array<int, 12> lengths{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int length = lengths[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0);
    return day <= length;
}

void append_two(std::string& out, std::int64_t value) {
    out.push_back(static_cast<char>('0' + value / 10));
    out.push_back(static_cast<char>('0' + value % 10));
}

}  // namespace

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned shifted_month = (5 * day_of_year + 2) / 153;
    const unsigned day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const unsigned month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400 + (month <= 2 ? 1 : 0);
    return {year, month, day};
}

CivilDate civil_from_timestamp(Timestamp time) noexcept {
    const std::int64_t days = time >= 0 ? time / seconds_per_day : (time - (seconds_per_day - 1)) / seconds_per_day;
    return civil_from_days(days);
}

std::optional<Timestamp> parse_timestamp(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    // All digits: a compact date (8 digits) or Unix time in seconds (9 to 11) or milliseconds (12 to 14).
    bool all_digits = true;
    for (const char c : text) {
        all_digits = all_digits && is_digit(c);
    }
    if (all_digits) {
        std::size_t position = 0;
        if (text.size() == 8) {
            const auto year = digits(text, position, 4);
            const auto month = digits(text, position, 2);
            const auto day = digits(text, position, 2);
            if (!year || !month || !day || !valid_date(*year, *month, *day)) {
                return std::nullopt;
            }
            return days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) * seconds_per_day;
        }
        if (text.size() >= 9 && text.size() <= 14) {
            const auto value = digits(text, position, text.size());
            if (!value) {
                return std::nullopt;
            }
            return text.size() <= 11 ? *value : *value / 1000;
        }
        return std::nullopt;
    }

    std::size_t position = 0;
    const auto year = digits(text, position, 4);
    if (!year || position >= text.size()) {
        return std::nullopt;
    }
    const char separator = text[position];
    if (separator != '-' && separator != '/' && separator != '.') {
        return std::nullopt;
    }
    ++position;
    const auto month = digits(text, position, 2);
    if (!month || position >= text.size() || text[position] != separator) {
        return std::nullopt;
    }
    ++position;
    const auto day = digits(text, position, 2);
    if (!day || !valid_date(*year, *month, *day)) {
        return std::nullopt;
    }
    Timestamp time =
        days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) * seconds_per_day;
    if (position == text.size()) {
        return time;
    }

    if (text[position] != ' ' && text[position] != 'T') {
        return std::nullopt;
    }
    ++position;
    const auto hour = digits(text, position, 2);
    if (!hour || position >= text.size() || text[position] != ':') {
        return std::nullopt;
    }
    ++position;
    const auto minute = digits(text, position, 2);
    if (!minute || *hour > 23 || *minute > 59) {
        return std::nullopt;
    }
    std::int64_t second = 0;
    if (position < text.size() && text[position] == ':') {
        ++position;
        const auto parsed = digits(text, position, 2);
        if (!parsed || *parsed > 60) {
            return std::nullopt;
        }
        second = *parsed;
        if (position < text.size() && text[position] == '.') {  // fractions of a second are dropped
            ++position;
            while (position < text.size() && is_digit(text[position])) {
                ++position;
            }
        }
    }
    time += *hour * 3600 + *minute * 60 + second;

    if (position < text.size() && text[position] == 'Z') {
        ++position;
    } else if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        const std::int64_t sign = text[position] == '+' ? 1 : -1;
        ++position;
        const auto offset_hours = digits(text, position, 2);
        if (!offset_hours) {
            return std::nullopt;
        }
        if (position < text.size() && text[position] == ':') {
            ++position;
        }
        const auto offset_minutes = digits(text, position, 2);
        if (!offset_minutes || *offset_hours > 23 || *offset_minutes > 59) {
            return std::nullopt;
        }
        time -= sign * (*offset_hours * 3600 + *offset_minutes * 60);
    }
    if (position != text.size()) {
        return std::nullopt;
    }
    return time;
}

std::string format_date(Timestamp time) {
    const CivilDate date = civil_from_timestamp(time);
    std::string out;
    out.reserve(10);
    append_two(out, date.year / 100);
    append_two(out, date.year % 100);
    out.push_back('-');
    append_two(out, date.month);
    out.push_back('-');
    append_two(out, date.day);
    return out;
}

std::string format_timestamp(Timestamp time) {
    std::string out = format_date(time);
    const std::int64_t of_day = ((time % seconds_per_day) + seconds_per_day) % seconds_per_day;
    if (of_day == 0) {
        return out;
    }
    out.push_back(' ');
    append_two(out, of_day / 3600);
    out.push_back(':');
    append_two(out, of_day / 60 % 60);
    if (of_day % 60 != 0) {
        out.push_back(':');
        append_two(out, of_day % 60);
    }
    return out;
}

double years_between(Timestamp from, Timestamp to) noexcept {
    return static_cast<double>(to - from) / (days_per_year * static_cast<double>(seconds_per_day));
}

}  // namespace nanotrade
