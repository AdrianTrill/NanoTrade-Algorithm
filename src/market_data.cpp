#include "nanotrade/market_data.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace nanotrade {
namespace {

std::string trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) {
        return !is_space(ch);
    }));

    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) {
        return !is_space(ch);
    }).base(), value.end());

    return value;
}

std::string normalizeHeaderName(const std::string& raw) {
    std::string normalized;
    normalized.reserve(raw.size());

    for (unsigned char ch : raw) {
        if (!std::isspace(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    return normalized;
}

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::stringstream stream(line);
    std::string token;

    while (std::getline(stream, token, ',')) {
        tokens.push_back(trim(token));
    }

    return tokens;
}

bool isNullToken(const std::string& token) {
    return token.empty() || token == "null" || token == "NULL";
}

double parseDouble(const std::string& token, std::size_t line_number, const std::string& column_name) {
    try {
        return std::stod(token);
    } catch (const std::exception&) {
        throw std::runtime_error(
            "Invalid numeric value in column '" + column_name + "' on CSV line " + std::to_string(line_number)
        );
    }
}

std::int64_t parseInt64(const std::string& token, std::size_t line_number, const std::string& column_name) {
    try {
        return std::stoll(token);
    } catch (const std::exception&) {
        throw std::runtime_error(
            "Invalid integer value in column '" + column_name + "' on CSV line " + std::to_string(line_number)
        );
    }
}

std::size_t requireColumn(
    const std::unordered_map<std::string, std::size_t>& header_map,
    const std::string& column_name
) {
    const auto it = header_map.find(column_name);
    if (it == header_map.end()) {
        throw std::runtime_error("Missing required CSV column: " + column_name);
    }
    return it->second;
}

}  // namespace

std::vector<Bar> MarketDataLoader::loadCsvFile(const std::string& file_path) {
    std::ifstream input(file_path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open CSV file: " + file_path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return loadCsvString(buffer.str());
}

std::vector<Bar> MarketDataLoader::loadCsvString(const std::string& csv_content) {
    std::stringstream stream(csv_content);
    std::string line;

    if (!std::getline(stream, line)) {
        throw std::runtime_error("CSV input is empty.");
    }

    const auto header_tokens = splitCsvLine(line);
    if (header_tokens.empty()) {
        throw std::runtime_error("CSV header is empty.");
    }

    std::unordered_map<std::string, std::size_t> header_map;
    for (std::size_t i = 0; i < header_tokens.size(); ++i) {
        header_map.emplace(normalizeHeaderName(header_tokens[i]), i);
    }

    const std::size_t date_index = requireColumn(header_map, "date");
    const std::size_t open_index = requireColumn(header_map, "open");
    const std::size_t high_index = requireColumn(header_map, "high");
    const std::size_t low_index = requireColumn(header_map, "low");
    const std::size_t close_index = requireColumn(header_map, "close");
    const std::size_t volume_index = requireColumn(header_map, "volume");

    const std::size_t required_index = std::max(
        {date_index, open_index, high_index, low_index, close_index, volume_index}
    );

    std::vector<Bar> bars;
    std::size_t line_number = 1;

    while (std::getline(stream, line)) {
        ++line_number;

        if (trim(line).empty()) {
            continue;
        }

        const auto tokens = splitCsvLine(line);
        if (tokens.size() <= required_index) {
            throw std::runtime_error("Malformed CSV row on line " + std::to_string(line_number));
        }

        const std::string& open_token = tokens[open_index];
        const std::string& high_token = tokens[high_index];
        const std::string& low_token = tokens[low_index];
        const std::string& close_token = tokens[close_index];

        if (isNullToken(open_token) || isNullToken(high_token) || isNullToken(low_token) || isNullToken(close_token)) {
            continue;
        }

        Bar bar;
        bar.date = tokens[date_index];
        bar.open = parseDouble(open_token, line_number, "open");
        bar.high = parseDouble(high_token, line_number, "high");
        bar.low = parseDouble(low_token, line_number, "low");
        bar.close = parseDouble(close_token, line_number, "close");
        bar.volume = isNullToken(tokens[volume_index])
            ? 0
            : parseInt64(tokens[volume_index], line_number, "volume");

        bars.push_back(bar);
    }

    validateBars(bars);
    return bars;
}

void MarketDataLoader::validateBars(const std::vector<Bar>& bars) {
    if (bars.empty()) {
        throw std::invalid_argument("At least one market data bar is required.");
    }

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const Bar& bar = bars[i];

        if (bar.date.empty()) {
            throw std::invalid_argument("Bar date must not be empty.");
        }

        if (bar.open <= 0.0 || bar.high <= 0.0 || bar.low <= 0.0 || bar.close <= 0.0) {
            throw std::invalid_argument("OHLC values must be positive.");
        }

        if (bar.high < bar.low) {
            throw std::invalid_argument("Bar high must be greater than or equal to bar low.");
        }

        if (bar.high < std::max(bar.open, bar.close) || bar.low > std::min(bar.open, bar.close)) {
            throw std::invalid_argument("OHLC values must be internally consistent.");
        }

        if (bar.volume < 0) {
            throw std::invalid_argument("Volume must not be negative.");
        }

        if (i > 0 && bar.date <= bars[i - 1].date) {
            throw std::invalid_argument("Bars must be strictly ordered by ascending date.");
        }
    }
}

}  // namespace nanotrade
