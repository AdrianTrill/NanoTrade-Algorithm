#include "CSVReader.h"
#include <fstream>
#include <sstream>
#include <iostream>

std::vector<PriceData> CSVReader::readCSV(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filePath << std::endl;
        return {};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    return readFromString(buffer.str());
}

std::vector<PriceData> CSVReader::readFromString(const std::string& csvContent) const {
    std::vector<PriceData> data;
    std::stringstream ss(csvContent);
    std::string line;

    bool headerSkipped = false;

    while (std::getline(ss, line)) {
        // Skip header
        if (!headerSkipped) {
            headerSkipped = true;
            continue;
        }
        if (line.empty()) continue; // skip empty lines

        std::stringstream lineStream(line);
        std::string token;
        PriceData pd;

        // Date
        std::getline(lineStream, token, ',');
        pd.date = token;

        // Open
        if (!std::getline(lineStream, token, ',')) break;
        if (token.empty() || token == "null") { // Yahoo might return "null"
            continue;
        }
        pd.open = std::stod(token);

        // High
        if (!std::getline(lineStream, token, ',')) break;
        if (token.empty() || token == "null") {
            continue;
        }
        pd.high = std::stod(token);

        // Low
        if (!std::getline(lineStream, token, ',')) break;
        if (token.empty() || token == "null") {
            continue;
        }
        pd.low = std::stod(token);

        // Close
        if (!std::getline(lineStream, token, ',')) break;
        if (token.empty() || token == "null") {
            continue;
        }
        pd.close = std::stod(token);

        // Adj Close (we ignore or skip if you want to parse it)
        if (!std::getline(lineStream, token, ',')) break;
        // skip token

        // Volume
        if (!std::getline(lineStream, token, ',')) break;
        if (token.empty() || token == "null") {
            pd.volume = 0;
        } else {
            pd.volume = std::stol(token);
        }

        data.push_back(pd);
    }

    return data;
}
