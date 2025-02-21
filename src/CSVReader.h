#ifndef CSVREADER_H
#define CSVREADER_H

#include <string>
#include <vector>

struct PriceData {
    std::string date;
    double open;
    double high;
    double low;
    double close;
    long volume;
};

class CSVReader {
public:
    // Reads CSV from a file (not used in this example, but we keep it for reference)
    static std::vector<PriceData> readCSV(const std::string& filePath);

    // Reads CSV from a string buffer (used after we download the data)
    std::vector<PriceData> readFromString(const std::string& csvContent) const;
};

#endif
