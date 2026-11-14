#ifndef NANOTRADE_JSON_HPP
#define NANOTRADE_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanotrade {

// A parsed JSON document. Objects keep their keys in order, and a repeated key keeps its last value, as in JavaScript.
class Json {
public:
    enum class Type : std::uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Json() = default;
    Json(bool value);  // NOLINT(google-explicit-constructor): builders read better without the noise
    Json(double value);
    Json(int value);
    Json(const char* value);
    Json(std::string value);

    static Json array();
    static Json object();

    // Throws nanotrade::Error, whose position() is the byte offset of the problem.
    static Json parse(std::string_view text);

    Type type() const noexcept {
        return type_;
    }
    bool is_null() const noexcept {
        return type_ == Type::Null;
    }
    bool is_bool() const noexcept {
        return type_ == Type::Bool;
    }
    bool is_number() const noexcept {
        return type_ == Type::Number;
    }
    bool is_string() const noexcept {
        return type_ == Type::String;
    }
    bool is_array() const noexcept {
        return type_ == Type::Array;
    }
    bool is_object() const noexcept {
        return type_ == Type::Object;
    }

    bool as_bool() const noexcept {
        return boolean_;
    }
    double as_number() const noexcept {
        return number_;
    }
    const std::string& as_string() const noexcept {
        return string_;
    }
    const std::vector<Json>& items() const noexcept {  // an array's elements, or an object's values
        return items_;
    }
    const std::vector<std::string>& keys() const noexcept {
        return keys_;
    }
    std::size_t size() const noexcept {
        return items_.size();
    }

    const Json* find(std::string_view key) const noexcept;  // nullptr unless an object with that key

    Json& push(Json value);                  // appends to an array
    Json& set(std::string key, Json value);  // sets an object's key, replacing any value it had

    std::string dump() const;  // compact, numbers in their shortest round-trip form

private:
    Type type_{Type::Null};
    bool boolean_{false};
    double number_{0.0};
    std::string string_;
    std::vector<std::string> keys_;
    std::vector<Json> items_;
};

// Streams JSON text. Commas and nesting are tracked, so callers only open, write, and close.
class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) {
        return value(std::string_view(text));
    }
    JsonWriter& value(const std::string& text) {
        return value(std::string_view(text));
    }
    JsonWriter& value(bool flag);
    JsonWriter& value(int number);
    JsonWriter& value(std::int64_t number);
    JsonWriter& value(std::size_t number);
    JsonWriter& value(double number, int digits = 0);  // digits > 0: that many significant digits
    JsonWriter& null();
    JsonWriter& raw(std::string_view json);  // a value that is already JSON
    JsonWriter& numbers(std::span<const double> values, int digits = 0);

    template <typename T>
    JsonWriter& field(std::string_view name, const T& v) {
        key(name);
        return value(v);
    }
    JsonWriter& field(std::string_view name, double v, int digits) {
        key(name);
        return value(v, digits);
    }

    const std::string& str() const noexcept {
        return out_;
    }
    std::string take() noexcept {
        return std::move(out_);
    }

private:
    void separate();

    std::string out_;
    std::vector<bool> first_;  // per open container: nothing written yet
    bool after_key_{false};
};

void append_json_string(std::string& out, std::string_view text);

}  // namespace nanotrade

#endif
