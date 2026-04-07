#ifndef NANOTRADE_INDICATORS_H
#define NANOTRADE_INDICATORS_H

#include <cstddef>
#include <vector>

#include "nanotrade/market_data.h"

namespace nanotrade::indicators {

using Series = std::vector<double>;

bool isDefined(double value);

Series simpleMovingAverage(const std::vector<Bar>& bars, std::size_t period);
Series exponentialMovingAverage(const std::vector<Bar>& bars, std::size_t period);
Series momentum(const std::vector<Bar>& bars, std::size_t period);
Series relativeStrengthIndex(const std::vector<Bar>& bars, std::size_t period);

}  // namespace nanotrade::indicators

#endif
