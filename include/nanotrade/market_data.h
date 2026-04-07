#ifndef NANOTRADE_MARKET_DATA_H
#define NANOTRADE_MARKET_DATA_H

#include <cstdint>
#include <string>
#include <vector>

namespace nanotrade {

struct Bar {
    std::string date;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    std::int64_t volume = 0;
};

class MarketDataLoader {
public:
    static std::vector<Bar> loadCsvFile(const std::string& file_path);
    static std::vector<Bar> loadCsvString(const std::string& csv_content);
    static void validateBars(const std::vector<Bar>& bars);
};

}  // namespace nanotrade

#endif
