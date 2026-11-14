#ifndef NANOTRADE_ERROR_HPP
#define NANOTRADE_ERROR_HPP

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace nanotrade {

// Every error NanoTrade reports about its input. `field` names the request field at fault, as a dotted path such as
// "strategy.params.fast", and for errors in an expression, `position` and `length` locate the offending characters,
// so a caller can point at them.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message,
                   std::string field = {},
                   std::size_t position = npos,
                   std::size_t length = 0)
        : std::runtime_error(message), field_(std::move(field)), position_(position), length_(length) {}

    const std::string& field() const noexcept {
        return field_;
    }
    std::size_t position() const noexcept {
        return position_;
    }
    std::size_t length() const noexcept {
        return length_;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    std::string field_;
    std::size_t position_;
    std::size_t length_;
};

}  // namespace nanotrade

#endif
