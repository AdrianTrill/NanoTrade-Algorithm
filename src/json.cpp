#include "nanotrade/json.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/text.hpp"

#include <cstdint>
#include <utility>

namespace nanotrade {
namespace {

constexpr std::size_t max_depth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json document() {
        skip_space();
        Json value = parse_value(0);
        skip_space();
        if (position_ != text_.size()) {
            fail("unexpected text after the JSON value");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw Error("invalid JSON at byte " + std::to_string(position_) + ": " + message, {}, position_, 1);
    }

    void skip_space() noexcept {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool consume(std::string_view literal) noexcept {
        if (text_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    Json parse_value(std::size_t depth) {
        if (depth > max_depth) {
            fail("nested too deeply");
        }
        if (position_ >= text_.size()) {
            fail("unexpected end of input");
        }
        const char c = text_[position_];
        if (c == '{') {
            return parse_object(depth);
        }
        if (c == '[') {
            return parse_array(depth);
        }
        if (c == '"') {
            return Json(parse_string());
        }
        if (consume("true")) {
            return Json(true);
        }
        if (consume("false")) {
            return Json(false);
        }
        if (consume("null")) {
            return {};
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return Json(parse_number());
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    Json parse_object(std::size_t depth) {
        ++position_;
        Json object = Json::object();
        skip_space();
        if (consume("}")) {
            return object;
        }
        for (;;) {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("expected a key in quotes");
            }
            std::string key = parse_string();
            skip_space();
            if (!consume(":")) {
                fail("expected ':' after a key");
            }
            skip_space();
            object.set(std::move(key), parse_value(depth + 1));
            skip_space();
            if (consume(",")) {
                continue;
            }
            if (consume("}")) {
                return object;
            }
            fail("expected ',' or '}' in an object");
        }
    }

    Json parse_array(std::size_t depth) {
        ++position_;
        Json array = Json::array();
        skip_space();
        if (consume("]")) {
            return array;
        }
        for (;;) {
            skip_space();
            array.push(parse_value(depth + 1));
            skip_space();
            if (consume(",")) {
                continue;
            }
            if (consume("]")) {
                return array;
            }
            fail("expected ',' or ']' in an array");
        }
    }

    double parse_number() {
        const std::size_t start = position_;
        if (text_[position_] == '-') {
            ++position_;
        }
        const auto digits = [&] {
            const std::size_t first = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            return position_ - first;
        };
        const std::size_t integer_start = position_;
        if (digits() == 0) {
            fail("expected a digit");
        }
        if (text_[integer_start] == '0' && position_ - integer_start > 1) {
            position_ = integer_start;
            fail("numbers can't have leading zeros");
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (digits() == 0) {
                fail("expected a digit after the decimal point");
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (digits() == 0) {
                fail("expected a digit in the exponent");
            }
        }
        const auto value = text::parse_number(text_.substr(start, position_ - start));
        if (!value) {
            position_ = start;
            fail("number out of range");
        }
        return *value;
    }

    unsigned parse_hex4() {
        if (position_ + 4 > text_.size()) {
            fail("incomplete \\u escape");
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            code <<= 4U;
            if (c >= '0' && c <= '9') {
                code |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                --position_;
                fail("invalid \\u escape");
            }
        }
        return code;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    std::string parse_string() {
        ++position_;
        std::string out;
        for (;;) {
            // Copy the run up to the next quote, backslash, or control character in one go: CSV uploads arrive as
            // strings of many megabytes.
            const std::size_t run_start = position_;
            while (position_ < text_.size()) {
                const auto c = static_cast<unsigned char>(text_[position_]);
                if (c == '"' || c == '\\' || c < 0x20U) {
                    break;
                }
                ++position_;
            }
            out.append(text_.substr(run_start, position_ - run_start));
            if (position_ >= text_.size()) {
                fail("unterminated string");
            }
            const char c = text_[position_];
            if (c == '"') {
                ++position_;
                return out;
            }
            if (c != '\\') {
                fail("control characters must be escaped in strings");
            }
            ++position_;
            if (position_ >= text_.size()) {
                fail("unterminated escape");
            }
            const char escape = text_[position_++];
            switch (escape) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned code = parse_hex4();
                    if (code >= 0xD800U && code <= 0xDBFFU) {
                        if (!consume("\\u")) {
                            fail("unpaired surrogate");
                        }
                        const unsigned low = parse_hex4();
                        if (low < 0xDC00U || low > 0xDFFFU) {
                            fail("invalid surrogate pair");
                        }
                        code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
                    } else if (code >= 0xDC00U && code <= 0xDFFFU) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    --position_;
                    fail("invalid escape");
            }
        }
    }

    std::string_view text_;
    std::size_t position_{0};
};

void dump_to(std::string& out, const Json& value) {
    switch (value.type()) {
        case Json::Type::Null:
            out += "null";
            break;
        case Json::Type::Bool:
            out += value.as_bool() ? "true" : "false";
            break;
        case Json::Type::Number:
            text::append_number(out, value.as_number());
            break;
        case Json::Type::String:
            append_json_string(out, value.as_string());
            break;
        case Json::Type::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                dump_to(out, value.items()[i]);
            }
            out.push_back(']');
            break;
        case Json::Type::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                append_json_string(out, value.keys()[i]);
                out.push_back(':');
                dump_to(out, value.items()[i]);
            }
            out.push_back('}');
            break;
    }
}

}  // namespace

Json::Json(bool value) : type_(Type::Bool), boolean_(value) {}
Json::Json(double value) : type_(Type::Number), number_(value) {}
Json::Json(int value) : type_(Type::Number), number_(value) {}
Json::Json(const char* value) : type_(Type::String), string_(value) {}
Json::Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

Json Json::array() {
    Json value;
    value.type_ = Type::Array;
    return value;
}

Json Json::object() {
    Json value;
    value.type_ = Type::Object;
    return value;
}

Json Json::parse(std::string_view text) {
    return Parser(text).document();
}

const Json* Json::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            return &items_[i];
        }
    }
    return nullptr;
}

Json& Json::push(Json value) {
    items_.push_back(std::move(value));
    return *this;
}

Json& Json::set(std::string key, Json value) {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(value);
            return *this;
        }
    }
    keys_.push_back(std::move(key));
    items_.push_back(std::move(value));
    return *this;
}

std::string Json::dump() const {
    std::string out;
    dump_to(out, *this);
    return out;
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[static_cast<unsigned char>(c) >> 4U]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0xFU]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void JsonWriter::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) {
            out_.push_back(',');
        }
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::begin_object() {
    separate();
    out_.push_back('{');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    out_.push_back('}');
    first_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    separate();
    out_.push_back('[');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    out_.push_back(']');
    first_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    separate();
    append_json_string(out_, name);
    out_.push_back(':');
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    separate();
    append_json_string(out_, text);
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    separate();
    out_ += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(int number) {
    separate();
    out_ += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number) {
    separate();
    out_ += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(std::size_t number) {
    separate();
    out_ += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(double number, int digits) {
    separate();
    text::append_number(out_, number, digits);
    return *this;
}

JsonWriter& JsonWriter::null() {
    separate();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::raw(std::string_view json) {
    separate();
    out_ += json;
    return *this;
}

JsonWriter& JsonWriter::numbers(std::span<const double> values, int digits) {
    begin_array();
    for (const double v : values) {
        if (!first_.back()) {
            out_.push_back(',');
        }
        first_.back() = false;
        text::append_number(out_, v, digits);
    }
    return end_array();
}

}  // namespace nanotrade
