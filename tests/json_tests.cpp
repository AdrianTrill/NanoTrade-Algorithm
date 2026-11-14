// Json::parse against the JSON grammar: every type, whitespace, nesting and its depth limit, numbers from the smallest
// double to the largest and past them, alike on every platform, and strings with every escape and surrogate pairs;
// the errors, each with the byte it points at; repeated keys; objects of 200,000 keys, in linear time; round trips
// through dump, and its shortest numbers; JsonWriter's nesting, escaping, fields, and numbers, with and without a digit
// limit; the builders' types; and the accessors on values of the wrong type.

#include "check.hpp"

#include "nanotrade/error.hpp"
#include "nanotrade/json.hpp"
#include "nanotrade/random.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nanotrade::Error;
using nanotrade::Json;
using nanotrade::JsonWriter;
using nanotrade::Random;

constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double largest = std::numeric_limits<double>::max();
constexpr double smallest_normal = std::numeric_limits<double>::min();
constexpr double smallest = std::numeric_limits<double>::denorm_min();

// ---- Helpers ----

std::string text_of(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

// `text` as a JSON string, so control characters show in messages, and cut short when it is long.
std::string shown(std::string_view text) {
    return Json(std::string(text.substr(0, 80)) + (text.size() > 80 ? "..." : "")).dump();
}

// Json::parse, naming the text when it throws.
Json parsed(std::string_view text) {
    try {
        return Json::parse(text);
    } catch (const Error& error) {
        throw check::Failure(shown(text) + " didn't parse: " + error.what());
    }
}

double number_of(std::string_view text) {
    const Json value = parsed(text);
    check::expect(value.is_number(), shown(text) + " is a number");
    return value.as_number();
}

std::string string_of(std::string_view text) {
    const Json value = parsed(text);
    check::expect(value.is_string(), shown(text) + " is a string");
    return value.as_string();
}

struct Problem {
    std::string message;
    std::size_t position;
    std::size_t length;
    std::string field;
};

// What Json::parse throws for `text`.
Problem problem_with(std::string_view text) {
    try {
        Json::parse(text);
    } catch (const Error& error) {
        return {error.what(), error.position(), error.length(), error.field()};
    }
    throw check::Failure(shown(text) + " parsed, but it isn't JSON");
}

// Parsing `text` fails at byte `position`, with a message that says so and then says `phrase`.
void expect_error(std::string_view text, std::size_t position, std::string_view phrase) {
    const Problem problem = problem_with(text);
    const std::string what = shown(text);
    check::expect_equal(problem.position, position, what + ": the position");
    const std::string start = "invalid JSON at byte " + std::to_string(position) + ": ";
    check::expect(
        problem.message.starts_with(start) && problem.message.find(phrase) != std::string::npos,
        what + ": \"" + problem.message + "\" should be \"" + start + "...\" and say \"" + std::string(phrase) + "\"");
    check::expect_equal(problem.length, std::size_t{1}, what + ": the length");
    check::expect(problem.field.empty(), what + ": no field to blame");
}

// Whether a and b are the same document: the same types, values, and keys, in the same order. The accessors of other
// types give neutral values (see accessors_on_the_wrong_type), so comparing them all compares what each type holds.
bool same(const Json& a, const Json& b) {
    if (a.type() != b.type() || a.as_bool() != b.as_bool() || a.as_number() != b.as_number() ||
        a.as_string() != b.as_string() || a.keys() != b.keys() || a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same(a.items()[i], b.items()[i])) {
            return false;
        }
    }
    return true;
}

// Whether `value`'s keys are `names`, in order.
bool keys_are(const Json& value, std::initializer_list<std::string_view> names) {
    return std::equal(value.keys().begin(), value.keys().end(), names.begin(), names.end());
}

// Short strings of quotes, backslashes, control characters, NUL, DEL, UTF-8, and a byte that isn't UTF-8.
std::string random_text(Random& random) {
    static const std::string_view pieces[] = {"\"",
                                              "\\",
                                              "/",
                                              "\n",
                                              "\t",
                                              "\b",
                                              "\f",
                                              "\r",
                                              std::string_view("\0", 1),
                                              "\x01",
                                              "\x1f",
                                              "\x7f",
                                              "a",
                                              "Z",
                                              " ",
                                              "\xc3\xa9",
                                              "\xe2\x82\xac",
                                              "\xf0\x9f\x98\x80",
                                              "\xff"};
    std::string text;
    const std::uint64_t length = random.below(9);
    for (std::uint64_t i = 0; i < length; ++i) {
        text += pieces[random.below(std::size(pieces))];
    }
    return text;
}

// Half the time any finite double, from its bits; otherwise a decimal with three places, as data holds.
double random_number(Random& random) {
    for (;;) {
        const double value =
            random.below(2) == 0
                ? std::bit_cast<double>(random.next())
                : static_cast<double>(static_cast<std::int64_t>(random.below(2000001)) - 1000000) / 1000.0;
        if (std::isfinite(value)) {
            return value;
        }
    }
}

// A random document, `depth` levels deep at most.
Json random_document(Random& random, int depth) {
    const std::uint64_t kind = random.below(depth > 0 ? 6 : 4);
    if (kind == 0) {
        return {};
    }
    if (kind == 1) {
        return {random.below(2) == 1};
    }
    if (kind == 2) {
        return {random_number(random)};
    }
    if (kind == 3) {
        return {random_text(random)};
    }
    Json container = kind == 4 ? Json::array() : Json::object();
    const std::uint64_t size = random.below(5);
    for (std::uint64_t i = 0; i < size; ++i) {
        if (kind == 4) {
            container.push(random_document(random, depth - 1));
        } else {
            container.set(random_text(random), random_document(random, depth - 1));
        }
    }
    return container;
}

// The fewest significant digits "%.*g" needs for `value` to read back as itself, and that text.
std::string shortest_printf(double value) {
    std::array<char, 40> buffer{};
    for (int digits = 1; digits <= 17; ++digits) {
        std::snprintf(buffer.data(), buffer.size(), "%.*g", digits, value);
        if (std::strtod(buffer.data(), nullptr) == value) {
            break;
        }
    }
    return buffer.data();
}

std::string written(double value, int digits = 0) {
    JsonWriter writer;
    writer.value(value, digits);
    return writer.str();
}

// ---- Parsing ----

void every_type() {
    check::expect(parsed("null").is_null(), "null");
    check::expect(parsed("true").is_bool() && parsed("true").as_bool(), "true");
    check::expect(parsed("false").is_bool() && !parsed("false").as_bool(), "false");
    check::expect_equal(number_of("42"), 42.0, "a number");
    check::expect_equal(string_of(R"("text")"), std::string("text"), "a string");
    check::expect(parsed("null").type() == Json::Type::Null && parsed("true").type() == Json::Type::Bool &&
                      parsed("1").type() == Json::Type::Number && parsed(R"("")").type() == Json::Type::String &&
                      parsed("[]").type() == Json::Type::Array && parsed("{}").type() == Json::Type::Object,
                  "type() names each");
    // Each value is exactly one type.
    for (const char* text : {"null", "false", "0", R"("")", "[]", "{}"}) {
        const Json value = parsed(text);
        const int types = static_cast<int>(value.is_null()) + static_cast<int>(value.is_bool()) +
                          static_cast<int>(value.is_number()) + static_cast<int>(value.is_string()) +
                          static_cast<int>(value.is_array()) + static_cast<int>(value.is_object());
        check::expect_equal(types, 1, std::string(text) + " is one type");
    }

    const Json array = parsed(R"([1, "two", null, true, [], {}])");
    check::expect(array.is_array() && array.size() == 6, "an array of six");
    const std::vector<Json>& items = array.items();
    check::expect(items[0].as_number() == 1.0 && items[1].as_string() == "two" && items[2].is_null() &&
                      items[3].as_bool() && items[4].is_array() && items[5].is_object(),
                  "an array's items, in order");
    check::expect(array.keys().empty(), "an array has no keys");

    const Json object = parsed(R"({"b": 1, "a": [2], "c": {"d": null}})");
    check::expect(object.is_object() && object.size() == 3, "an object of three");
    check::expect(keys_are(object, {"b", "a", "c"}), "keys in the order written");
    check::expect(object.items()[0].as_number() == 1.0 && object.items()[1].is_array(), "items are the values");
    const Json* a = object.find("a");
    check::expect(a != nullptr && a->size() == 1 && a->items()[0].as_number() == 2.0, "find");
    const Json* c = object.find("c");
    check::expect(c != nullptr && c->find("d") != nullptr && c->find("d")->is_null(), "find within find");
    check::expect(object.find("z") == nullptr && object.find("") == nullptr, "a missing key");

    check::expect(parsed("[]").size() == 0 && parsed("{}").size() == 0, "empty containers");
    check::expect(string_of(R"("")").empty(), "an empty string");
    check::expect_equal(parsed(R"({"": 1})").keys().front(), std::string(), "an empty key");
}

// JSON's whitespace is space, tab, line feed, and carriage return, before and after any token; nothing else is.
void whitespace() {
    const std::string space = " \t\n\r";
    const std::string text = space + "{" + space + R"("a")" + space + ":" + space + "[" + space + "1" + space + "," +
                             space + "true" + space + "]" + space + "," + space + R"("b")" + space + ":" + space +
                             "null" + space + "}" + space;
    check::expect_equal(parsed(text).dump(), std::string(R"({"a":[1,true],"b":null})"), "around every token");
    check::expect_equal(string_of(R"( " a  b " )"), std::string(" a  b "), "inside strings, spaces are text");
    // Not whitespace: form feed, vertical tab, a no-break space, and a byte order mark.
    expect_error("\f1", 0, "unexpected character");
    expect_error("\v1", 0, "unexpected character");
    expect_error("1\f", 1, "unexpected text after the JSON value");
    expect_error(
        "[1,\xc2\xa0"
        "2]",
        3,
        "unexpected character");
    expect_error("\xef\xbb\xbf{}", 0, "unexpected character");
}

// Values nest to 64 levels below the top. The limit counts values, so 65 brackets parse around nothing, but not
// around a number; past it, the error points at the first value too deep, and a million brackets fail there too,
// without recursing a million deep.
void nesting_and_its_limit() {
    const std::string mixed = R"({"a":[{"b":[[],{"c":[1,[2,{"d":"e"}]]}]}]})";
    const Json document = parsed(mixed);
    check::expect_equal(document.dump(), mixed, "a mixed nesting");
    const Json& c = *document.find("a")->items()[0].find("b")->items()[1].find("c");
    check::expect_equal(c.items()[1].items()[1].find("d")->as_string(), std::string("e"), "found at the bottom");

    const auto arrays = [](std::size_t levels, std::string_view inside) {
        return std::string(levels, '[') + std::string(inside) + std::string(levels, ']');
    };
    check::expect_equal(parsed(arrays(64, "1")).dump(), arrays(64, "1"), "a number 64 levels down");
    check::expect_equal(parsed(arrays(65, "")).dump(), arrays(65, ""), "65 brackets around nothing");
    expect_error(arrays(65, "1"), 65, "nested too deeply");
    expect_error(arrays(66, ""), 65, "nested too deeply");

    const auto objects = [](std::size_t levels) {
        std::string text;
        for (std::size_t i = 0; i < levels; ++i) {
            text += R"({"a":)";
        }
        return text + "1" + std::string(levels, '}');
    };
    check::expect_equal(parsed(objects(64)).dump(), objects(64), "objects 64 levels deep");
    expect_error(objects(65), 5 * 65, "nested too deeply");
    expect_error(std::string(1000000, '['), 65, "nested too deeply");
}

// Integers, fractions, and exponents in every form the grammar allows, correctly rounded; -0 with its sign; and the
// extremes. 1e400, or anything else past the largest double, is an error, "number out of range", at the number's
// first byte: JSON has no infinity, and the parser won't make one up.
void numbers() {
    struct Case {
        const char* text;
        double value;
    };
    const Case cases[] = {
        {"0", 0.0},
        {"1", 1.0},
        {"-1", -1.0},
        {"42", 42.0},
        {"123456789", 123456789.0},
        {"0.5", 0.5},
        {"-0.25", -0.25},
        {"3.14159", 3.14159},
        {"0.1", 0.1},
        {"0.30000000000000004", 0.1 + 0.2},
        {"1e3", 1000.0},
        {"1E3", 1000.0},
        {"1e+3", 1000.0},
        {"1e-3", 0.001},
        {"1.5e2", 150.0},
        {"-2.5E-3", -0.0025},
        {"1e007", 1e7},
        {"0e0", 0.0},
        {"0E+0", 0.0},
        {"0.0e-999", 0.0},
        {"0e999999", 0.0},
        {"1e308", 1e308},
        // 2^53, its successor, which ties to the even 2^53, and 20 digits, to the nearest double.
        {"9007199254740992", 9007199254740992.0},
        {"9007199254740993", 9007199254740992.0},
        {"12345678901234567890", 12345678901234567168.0},
        // The largest double, and a decimal just under halfway to the next power of two, which rounds down to it.
        {"1.7976931348623157e308", largest},
        {"-1.7976931348623157e308", -largest},
        {"1.7976931348623158e308", largest},
        // The smallest normal double, the largest subnormal, and the smallest, which 3e-324 rounds up to.
        {"2.2250738585072014e-308", smallest_normal},
        {"2.2250738585072009e-308", 0x0.fffffffffffffp-1022},
        {"4.9406564584124654e-324", smallest},
        {"5e-324", smallest},
        {"-5e-324", -smallest},
        {"3e-324", smallest},
        // The exact value of the double 0.1, written out.
        {"0.1000000000000000055511151231257827021181583404541015625", 0.1},
    };
    for (const Case& number : cases) {
        check::expect_equal(number_of(number.text), number.value, number.text);
    }
    check::expect(number_of("-0") == 0.0 && std::signbit(number_of("-0")), "-0 keeps its sign");
    check::expect(std::signbit(number_of("-0.0e5")), "-0.0e5 too");
    check::expect(!std::signbit(number_of("0")), "0 has none");

    expect_error("1e400", 0, "number out of range");
    expect_error("-1e400", 0, "number out of range");
    expect_error("[1, 1e400]", 4, "number out of range");
    expect_error(R"({"big": 2e308})", 8, "number out of range");
    expect_error("1.7976931348623159e308", 0, "number out of range");
}

// Numbers at the ends of the range read alike on every platform, whether text::parse_number reads with std::from_chars
// (libstdc++, the MSVC STL) or with strtod (Apple's libc++): a literal too small for a double is +-0, as correctly
// rounding it gives, one too large is "number out of range", and a literal may be as long as it likes. The edges: a
// hair either side of half the smallest double, exponents past any integer type, and literals of 300 to 100,000
// digits. (Before, from_chars rejected underflow, and the strtod path any literal of 512 characters or more.)
void numbers_read_alike_everywhere() {
    struct Case {
        std::string text;
        double value;
    };
    const Case cases[] = {
        {"1e-400", 0.0},
        {"-1e-400", -0.0},
        {"2e-324", 0.0},
        {"2.4703282292062327e-324", 0.0},       // just under half the smallest double
        {"2.4703282292062328e-324", smallest},  // just over
        {"123.456e-327", 0.0},
        {"1e-2147483649", 0.0},
        {"-1e-99999999999999999999", -0.0},
        {"0.0e400", 0.0},
        {"0." + std::string(400, '0') + "1", 0.0},
        {"1" + std::string(308, '0'), 1e308},
        {"1." + std::string(100000, '0'), 1.0},
        {"0.1" + std::string(100000, '0') + "e-1", 0.01},
    };
    for (const Case& number : cases) {
        const double read = number_of(number.text);
        check::expect(read == number.value && std::signbit(read) == std::signbit(number.value),
                      shown(number.text) + " reads as " + text_of(read) + ", not " + text_of(number.value));
    }
    for (const std::string& text : {std::string("1e400"),
                                    std::string("-1e400"),
                                    std::string("1e+2147483649"),
                                    std::string("1e99999999999999999999"),
                                    "1" + std::string(309, '0'),
                                    "1" + std::string(400, '0')}) {
        expect_error(text, 0, "number out of range");
    }
}

// Every escape; \u escapes in either case, at the edges of one-, two-, and three-byte UTF-8; surrogate pairs, to four
// bytes; and NUL. Raw bytes from 0x20 up pass as they are, DEL and UTF-8 among them, and the parser doesn't validate
// UTF-8, so stray bytes pass too.
void strings() {
    check::expect_equal(string_of(R"("\" \\ \/ \b \f \n \r \t")"),
                        std::string("\" \\ / \b \f \n \r \t"),
                        "the two-character escapes");
    check::expect_equal(string_of(R"("\u0041\u00e9\u00E9")"),
                        std::string("A\xc3\xa9\xc3\xa9"),
                        "\\u escapes, in either case");
    check::expect(string_of(R"("a\u0000b")") == std::string("a\0b", 3), "\\u0000 is a NUL within the string");
    check::expect_equal(string_of(R"("\u007f\u0080\u07ff\u0800\uffff")"),
                        std::string("\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf"),
                        "the edges of one, two, and three bytes");
    check::expect_equal(string_of(R"("\ud83d\ude00")"), std::string("\xf0\x9f\x98\x80"), "U+1F600, a pair");
    check::expect_equal(string_of(R"("\uD834\uDD1E")"), std::string("\xf0\x9d\x84\x9e"), "U+1D11E, in capitals");
    check::expect_equal(string_of(R"("\ud800\udc00")"), std::string("\xf0\x90\x80\x80"), "U+10000, the first pair");
    check::expect_equal(string_of(R"("\udbff\udfff")"), std::string("\xf4\x8f\xbf\xbf"), "U+10FFFF, the last");
    check::expect_equal(string_of("\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\""),
                        std::string("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"),
                        "raw UTF-8");
    check::expect_equal(string_of("\"\x7f\""), std::string("\x7f"), "DEL needs no escape");
    check::expect_equal(string_of("\"\xff\xfe\x80\""), std::string("\xff\xfe\x80"), "bytes that aren't UTF-8 pass");
    // A long string, as CSV uploads arrive, comes through whole.
    std::string raw;
    std::string json = "\"";
    for (int i = 0; i < 50000; ++i) {
        raw += "2024-01-02,187.15\n";
        json += "2024-01-02,187.15\\n";
    }
    check::expect(string_of(json + "\"") == raw, "900,000 bytes");
}

// Malformed strings, each blamed on a byte: an invalid escape on its letter, a bad hex digit on itself, a short \u
// where its digits start, an unpaired or mismatched surrogate just past its escape, a raw control character on
// itself, and an unterminated string or escape on the end of the input.
void string_errors() {
    expect_error(R"("\ud83d")", 7, "unpaired surrogate");
    expect_error(R"("\ud83dx")", 7, "unpaired surrogate");
    expect_error(R"("\ude00")", 7, "unpaired surrogate");
    expect_error(R"("\ud83d\u0041")", 13, "invalid surrogate pair");
    expect_error(R"("\ud83d\ud83d")", 13, "invalid surrogate pair");
    expect_error(R"("\ud83d\u12")", 9, "incomplete \\u escape");
    expect_error(R"("\u12")", 3, "incomplete \\u escape");
    expect_error(R"("\u12G4")", 5, "invalid \\u escape");
    expect_error(R"("\uZZZZ")", 3, "invalid \\u escape");
    for (const char* text : {R"("\a")", R"("\x41")", R"("\'")", R"("\U0041")", R"("\0")", R"("\ ")"}) {
        expect_error(text, 2, "invalid escape");
    }
    expect_error("\"a\tb\"", 2, "control characters must be escaped in strings");
    expect_error("\"a\nb\"", 2, "control characters must be escaped in strings");
    expect_error("\"\x1f\"", 1, "control characters must be escaped in strings");
    expect_error(std::string_view("\"a\0b\"", 5), 2, "control characters must be escaped in strings");
    expect_error(R"("abc)", 4, "unterminated string");
    expect_error(R"(["abc)", 5, "unterminated string");
    expect_error(R"({"abc)", 5, "unterminated string");
    expect_error(R"("abc\)", 5, "unterminated escape");
}

// The rest of what isn't JSON, and the byte each error points at: trailing commas, keys not in double quotes, leading
// zeros, a lone minus and other numbers missing digits, text after the value, empty input, unclosed containers,
// missing colons, commas, and values, and literals JSON doesn't have.
void syntax_errors() {
    struct Case {
        std::string_view text;
        std::size_t position;
        std::string_view phrase;
    };
    const Case cases[] = {
        {"[1,2,]", 5, "unexpected character ']'"},
        {R"({"a":1,})", 7, "expected a key in quotes"},
        {"[,]", 1, "unexpected character ','"},
        {"[1,,2]", 3, "unexpected character ','"},
        {"{a:1}", 1, "expected a key in quotes"},
        {"{'a':1}", 1, "expected a key in quotes"},
        {"{1:1}", 1, "expected a key in quotes"},
        {"['a']", 1, "unexpected character '''"},
        {"01", 0, "numbers can't have leading zeros"},
        {"-01", 1, "numbers can't have leading zeros"},
        {"[00]", 1, "numbers can't have leading zeros"},
        {"007.5", 0, "numbers can't have leading zeros"},
        {"-", 1, "expected a digit"},
        {"[-]", 2, "expected a digit"},
        {"- 1", 1, "expected a digit"},
        {"-Infinity", 1, "expected a digit"},
        {".5", 0, "unexpected character '.'"},
        {"+1", 0, "unexpected character '+'"},
        {"1.", 2, "expected a digit after the decimal point"},
        {"1.e5", 2, "expected a digit after the decimal point"},
        {"1e", 2, "expected a digit in the exponent"},
        {"1e+", 3, "expected a digit in the exponent"},
        {"[1E-]", 4, "expected a digit in the exponent"},
        {"{} x", 3, "unexpected text after the JSON value"},
        {"1 2", 2, "unexpected text after the JSON value"},
        {"[1]]", 3, "unexpected text after the JSON value"},
        {"nullx", 4, "unexpected text after the JSON value"},
        {R"("a"b)", 3, "unexpected text after the JSON value"},
        {"1e5.5", 3, "unexpected text after the JSON value"},
        {"0x10", 1, "unexpected text after the JSON value"},
        {"1_000", 1, "unexpected text after the JSON value"},
        {std::string_view("1\0", 2), 1, "unexpected text after the JSON value"},
        {"", 0, "unexpected end of input"},
        {"   ", 3, "unexpected end of input"},
        {"[", 1, "unexpected end of input"},
        {"[1,", 3, "unexpected end of input"},
        {"[1,2", 4, "expected ',' or ']' in an array"},
        {"{", 1, "expected a key in quotes"},
        {R"({"a")", 4, "expected ':' after a key"},
        {R"({"a":)", 5, "unexpected end of input"},
        {R"({"a":1)", 6, "expected ',' or '}' in an object"},
        {R"({"a"})", 4, "expected ':' after a key"},
        {R"({"a" 1})", 5, "expected ':' after a key"},
        {R"({"a":})", 5, "unexpected character '}'"},
        {"[1 2]", 3, "expected ',' or ']' in an array"},
        {R"({"a":1 "b":2})", 7, "expected ',' or '}' in an object"},
        {"tru", 0, "unexpected character 't'"},
        {"True", 0, "unexpected character 'T'"},
        {"NULL", 0, "unexpected character 'N'"},
        {"nul", 0, "unexpected character 'n'"},
        {"NaN", 0, "unexpected character 'N'"},
        {"Infinity", 0, "unexpected character 'I'"},
        {"undefined", 0, "unexpected character 'u'"},
        {"// note\n1", 0, "unexpected character '/'"},
        {"/* note */ 1", 0, "unexpected character '/'"},
    };
    for (const Case& bad : cases) {
        expect_error(bad.text, bad.position, bad.phrase);
    }
}

// A repeated key keeps its last value, in the place the key first appeared, as JavaScript's JSON.parse does. The value
// is replaced whole, not merged; keys compare after their escapes are decoded; each object keeps its own keys; and
// set() does the same, so a document reads alike however it was made.
void duplicate_keys() {
    const Json object = parsed(R"({"a": 1, "b": 2, "a": 3})");
    check::expect(keys_are(object, {"a", "b"}), "one \"a\", where it first appeared");
    check::expect_equal(object.size(), std::size_t{2}, "two values");
    check::expect_equal(object.find("a")->as_number(), 3.0, "the last value");
    check::expect_equal(object.dump(), std::string(R"({"a":3,"b":2})"), "dumped once");
    check::expect_equal(parsed(R"({"a":1,"a":2,"a":3,"a":4})").dump(), std::string(R"({"a":4})"), "four times");
    check::expect_equal(parsed(R"({"a": {"x": 1, "y": 2}, "a": {"z": 3}})").dump(),
                        std::string(R"({"a":{"z":3}})"),
                        "replaced, not merged");
    check::expect_equal(parsed(R"({"a": [1], "a": "text"})").dump(),
                        std::string(R"({"a":"text"})"),
                        "even by another type");
    check::expect_equal(parsed(R"({"a": 1, "\u0061": 2})").dump(), std::string(R"({"a":2})"), "\"\\u0061\" is \"a\"");
    check::expect_equal(parsed(R"({"a": {"a": 1, "a": 2}, "b": {"a": 3}})").dump(),
                        std::string(R"({"a":{"a":2},"b":{"a":3}})"),
                        "each object on its own");
    Json built = Json::object();
    built.set("a", 1).set("b", 2).set("a", 3);
    check::expect(same(built, object), "set() replaces the same way");
    check::expect(same(parsed(object.dump()), object) && parsed(object.dump()).dump() == object.dump(),
                  "and a round trip moves nothing");
}

// Objects parse in time linear in their keys: 200,000 distinct ones take about 0.2 s in a debug build and 0.6 to 0.9 s
// under AddressSanitizer (measured), where a search through the keys so far for each new one took minutes; 2 s is
// allowed. Then with 20,000 of the keys repeated at the end, each keeps its last value in its first place.
void many_keys() {
    constexpr int count = 200000;
    const auto seconds_to_parse = [](const std::string& text, Json& document) {
        const auto start = std::chrono::steady_clock::now();
        document = Json::parse(text);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    std::string text = "{";
    for (int i = 0; i < count; ++i) {
        text += (i == 0 ? "\"k" : ",\"k") + std::to_string(i) + "\":" + std::to_string(i);
    }
    {
        Json distinct;
        const double seconds = seconds_to_parse(text + "}", distinct);
        check::expect(seconds < 2.0, "200,000 keys took " + text_of(seconds) + " s");
        check::expect_equal(distinct.size(), std::size_t{count}, "a value for each key");
        for (int i = 0; i < count; ++i) {
            if (distinct.keys()[i] != "k" + std::to_string(i) || distinct.items()[i].as_number() != i) {
                throw check::Failure("key " + std::to_string(i) + " is " + distinct.keys()[i]);
            }
        }
    }
    for (int i = 0; i < count; i += 10) {
        text += ",\"k" + std::to_string(i) + "\":" + std::to_string(-i - 1);
    }
    Json repeated;
    const double seconds = seconds_to_parse(text + "}", repeated);
    check::expect(seconds < 2.0, "200,000 keys and 20,000 repeats took " + text_of(seconds) + " s");
    check::expect_equal(repeated.size(), std::size_t{count}, "a value for each distinct key");
    for (int i = 0; i < count; ++i) {
        const double expected = i % 10 == 0 ? -i - 1 : i;
        if (repeated.keys()[i] != "k" + std::to_string(i) || repeated.items()[i].as_number() != expected) {
            throw check::Failure("key " + std::to_string(i) + " is " + repeated.keys()[i] + ", with " +
                                 text_of(repeated.items()[i].as_number()));
        }
    }
}

// ---- Round trips and dump ----

// dump(parse(text)) is text for compact documents, and parse(dump(x)) is x: for a document of every type, for every
// byte from NUL to 0xFF in a key and a value, and for 3,000 random documents.
void round_trips() {
    for (const char* text : {"null",
                             "true",
                             "false",
                             "0",
                             "-1.5",
                             "1e+21",
                             "5e-324",
                             R"("")",
                             R"("a\"b\\c\n\r\t\u0000\u0008\u000c\u001f/")",
                             "\"caf\xc3\xa9\"",
                             "[]",
                             "{}",
                             R"([1,"two",null,true,false,[[]],{}])",
                             R"({"a":{"b":[1,{"c":null}]},"":"an empty key","z":-0.001})"}) {
        check::expect_equal(parsed(text).dump(), std::string(text), "dump(parse(" + shown(text) + "))");
    }

    Json array = Json::array();
    array.push(1).push("two").push(Json::array()).push(Json::object()).push(Json());
    Json document = Json::object();
    document.set("null", Json())
        .set("yes", true)
        .set("no", false)
        .set("int", 42)
        .set("pi", 3.141592653589793)
        .set("smallest", smallest)
        .set("largest", -largest)
        .set("text", "line\nbreak \"quoted\" \\ \t tab")
        .set("array", array);
    check::expect(same(parsed(document.dump()), document), "a document of every type");

    std::string bytes;
    for (int c = 0; c < 256; ++c) {
        bytes.push_back(static_cast<char>(static_cast<unsigned char>(c)));
    }
    Json holder = Json::object();
    holder.set(bytes, bytes);
    const Json back = parsed(holder.dump());
    check::expect(back.keys().size() == 1 && back.keys()[0] == bytes && back.items()[0].as_string() == bytes,
                  "every byte, in a key and a value");

    Random random(2024);
    for (int i = 0; i < 3000; ++i) {
        const Json original = random_document(random, 4);
        const std::string text = original.dump();
        const Json copy = parsed(text);
        if (!same(copy, original) || copy.dump() != text) {
            throw check::Failure("random document " + std::to_string(i) + " changed: " + shown(text));
        }
    }
}

// dump writes each number in the fewest characters that read back as it: 0.1 as 0.1, and fixed or scientific
// notation, whichever is shorter, so a million is 1e+06. -0 is 0, and NaN and the infinities, which JSON can't carry,
// are null. Then 20,000 random doubles from their bits and 20,000 short decimals read back exactly, each no longer
// than the shortest "%.*g" that round-trips.
void shortest_numbers() {
    struct Case {
        double value;
        const char* text;
    };
    const Case cases[] = {
        {0.1, "0.1"},
        {0.1 + 0.2, "0.30000000000000004"},
        {1.0 / 3.0, "0.3333333333333333"},
        {2.0 / 3.0, "0.6666666666666666"},
        {-1.5, "-1.5"},
        {100.0, "100"},
        {1e6, "1e+06"},
        {123456789012.0, "123456789012"},
        {9007199254740992.0, "9007199254740992"},
        {12345678901234567168.0, "12345678901234567168"},
        {1e21, "1e+21"},
        {0.001, "0.001"},
        {0.0001, "1e-04"},
        {1e-7, "1e-07"},
        {largest, "1.7976931348623157e+308"},
        {smallest_normal, "2.2250738585072014e-308"},
        {smallest, "5e-324"},
        {0.0, "0"},
        {-0.0, "0"},
        {nan, "null"},
        {inf, "null"},
        {-inf, "null"},
    };
    for (const Case& number : cases) {
        check::expect_equal(Json(number.value).dump(), std::string(number.text), "dump of " + text_of(number.value));
    }

    Random random(7);
    const auto expect_shortest = [](double value) {
        const std::string text = Json(value).dump();
        const Json back = parsed(text);
        if (back.as_number() != value || text.size() > shortest_printf(value).size()) {
            throw check::Failure(text_of(value) + " dumps as " + text + ", which reads back as " +
                                 text_of(back.as_number()) + "; %g's shortest is " + shortest_printf(value));
        }
    };
    for (int i = 0; i < 20000; ++i) {
        const double value = std::bit_cast<double>(random.next());
        if (std::isfinite(value)) {
            expect_shortest(value);
        }
        const std::string decimal =
            std::to_string(random.below(100000000)) + "e" + std::to_string(static_cast<int>(random.below(61)) - 30);
        expect_shortest(std::strtod(decimal.c_str(), nullptr));
    }
}

// ---- JsonWriter ----

// Commas and colons where they belong, at any depth, with fields of every type, raw JSON, null, and numbers().
void writer_nesting_and_fields() {
    JsonWriter writer;
    writer.begin_object()
        .field("int", 1)
        .field("negative", -7)
        .field("bool", true)
        .field("text", "chars")
        .field("string", std::string("a string"))
        .field("view", std::string_view("a view"))
        .field("double", 2.5)
        .field("rounded", 3.14159, 3)
        .field("count", std::size_t{12})
        .field("wide", std::int64_t{-9000000000})
        .key("nothing")
        .null()
        .key("raw")
        .raw(R"({"x":[1,2]})")
        .key("series")
        .numbers(std::vector<double>{1.0, 2.5})
        .key("empty")
        .begin_array()
        .end_array()
        .key("nested")
        .begin_array()
        .begin_object()
        .end_object()
        .begin_array()
        .value(1)
        .begin_array()
        .end_array()
        .end_array()
        .raw("null")
        .value("last")
        .end_array()
        .end_object();
    const std::string expected =
        R"({"int":1,"negative":-7,"bool":true,"text":"chars","string":"a string","view":"a view","double":2.5,)"
        R"("rounded":3.14,"count":12,"wide":-9000000000,"nothing":null,"raw":{"x":[1,2]},"series":[1,2.5],)"
        R"("empty":[],"nested":[{},[1,[]],null,"last"]})";
    check::expect_equal(writer.str(), expected, "the document");
    const Json back = parsed(writer.str());
    check::expect(back.size() == 15 && back.find("wide")->as_number() == -9e9 && back.find("nested")->size() == 4,
                  "it parses back");

    JsonWriter top;
    top.begin_array().value(1).numbers(std::vector<double>{}).numbers(std::vector<double>{2.0}).value(3).end_array();
    check::expect_equal(top.str(), std::string("[1,[],[2],3]"), "numbers() among other values");
    JsonWriter alone;
    alone.value("only");
    check::expect_equal(alone.str(), std::string(R"("only")"), "a lone value");
    check::expect_equal(alone.take(), std::string(R"("only")"), "take() hands over the text");
}

// Keys and values are escaped alike: quote, backslash, \n, \r, and \t by name, \b, \f, and the other control
// characters as \u00xx, NUL among them; DEL, '/', and UTF-8 as they are. What is written reads back byte for byte.
void writer_escaping() {
    const std::string text = std::string("a\"b\\c\n\r\t\b\f\x01\x1f\x7f /\xc3\xa9") + std::string(1, '\0');
    JsonWriter writer;
    writer.begin_object().field(text, text).end_object();
    const std::string escaped = R"("a\"b\\c\n\r\t\u0008\u000c\u0001\u001f)"
                                "\x7f /\xc3\xa9"
                                R"(\u0000")";
    check::expect_equal(writer.str(), "{" + escaped + ":" + escaped + "}", "escaped");
    const Json back = parsed(writer.str());
    check::expect(back.keys().front() == text && back.items().front().as_string() == text, "and read back");

    std::string out;
    nanotrade::append_json_string(out, "tab\there");
    check::expect_equal(out, std::string(R"("tab\there")"), "append_json_string");
}

// Without a digit limit, the shortest text that reads back; with one, that many significant digits, rounded from the
// double's exact value, trailing zeros dropped, in %g's choice of notation. NaN and the infinities are null either way,
// and -0 is 0. Then, for random values and every limit up to 17, the text reads back within half a unit in its last
// digit.
void writer_numbers() {
    check::expect_equal(written(0.1), std::string("0.1"), "0.1");
    check::expect_equal(written(1e21), std::string("1e+21"), "1e21");
    check::expect_equal(written(-0.0), std::string("0"), "-0");
    check::expect_equal(written(smallest), std::string("5e-324"), "the smallest double");
    check::expect_equal(written(largest), std::string("1.7976931348623157e+308"), "the largest");
    struct Case {
        double value;
        int digits;
        const char* text;
    };
    const Case cases[] = {
        {3.14159265358979, 3, "3.14"},
        {3.14159265358979, 1, "3"},
        {2.5, 1, "2"},     // exactly halfway, to even
        {0.15, 1, "0.1"},  // 0.1499999999999999944...
        {100.0, 1, "1e+02"},
        {100.0, 3, "100"},
        {123456.0, 3, "1.23e+05"},
        {1e6, 17, "1000000"},
        {1e21, 30, "1000000000000000000000"},
        {0.1, 17, "0.10000000000000001"},
        {0.1, 20, "0.10000000000000000555"},
        {1.0 / 3.0, 30, "0.333333333333333314829616256247"},
        {smallest, 3, "4.94e-324"},
        {largest, 1, "2e+308"},
        {-0.0, 3, "0"},
        {nan, 3, "null"},
        {inf, 3, "null"},
        {-inf, 3, "null"},
    };
    for (const Case& number : cases) {
        check::expect_equal(written(number.value, number.digits),
                            std::string(number.text),
                            text_of(number.value) + " to " + std::to_string(number.digits) + " digits");
    }

    JsonWriter writer;
    writer.begin_array()
        .value(std::numeric_limits<int>::min())
        .value(std::numeric_limits<int>::max())
        .value(std::numeric_limits<std::int64_t>::min())
        .value(std::numeric_limits<std::size_t>::max())
        .end_array();
    check::expect_equal(
        writer.str(),
        "[-2147483648,2147483647,-9223372036854775808," + std::to_string(std::numeric_limits<std::size_t>::max()) + "]",
        "integers whole");
    const std::vector<double> series{1.5, nan, 2.0, 1.23456, -0.0, inf};
    JsonWriter all;
    all.numbers(series);
    JsonWriter three;
    three.numbers(series, 3);
    check::expect_equal(all.str(), std::string("[1.5,null,2,1.23456,0,null]"), "numbers()");
    check::expect_equal(three.str(), std::string("[1.5,null,2,1.23,0,null]"), "numbers() to 3 digits");

    Random random(11);
    for (int i = 0; i < 20000; ++i) {
        const double value = std::bit_cast<double>(random.next());
        const int digits = 1 + static_cast<int>(random.below(17));
        if (!std::isfinite(value) || value == 0.0) {
            continue;
        }
        const std::string text = written(value, digits);
        const double back = parsed(text).as_number();
        // Half a unit in the last of `digits` digits, relative to the value, is at most 5 / 10^digits.
        if (!(std::fabs(back - value) <= 5.0 * std::pow(10.0, -digits) * std::fabs(value))) {
            throw check::Failure(text_of(value) + " to " + std::to_string(digits) + " digits is " + text);
        }
    }
}

// More digits than a double has still write the number: every limit from 17 to 100 gives text that reads back as the
// double itself, its exact binary expansion as far as the limit goes, and any limit past 100 writes what 100 does.
// (Before, a 64-byte buffer ran out from 59 digits on, and the writer wrote all 64 bytes, NULs and all.)
void writer_many_digits() {
    for (const double value : {0.1, 1.0 / 3.0, 0.0001, 1e-300, 1e300, 123456.789, largest, -largest, smallest}) {
        for (int digits = 17; digits <= 100; ++digits) {
            const std::string text = written(value, digits);
            const std::string what = text_of(value) + " to " + std::to_string(digits) + " digits, " + shown(text);
            check::expect(text.find('\0') == std::string::npos, what + ", has a NUL");
            check::expect_equal(number_of(text), value, what + ", reads back");
        }
        check::expect_equal(written(value, 101), written(value, 100), text_of(value) + " to 101 digits");
        check::expect_equal(written(value, 1000), written(value, 100), text_of(value) + " to 1000 digits");
    }
    check::expect_equal(written(0.1, 100),
                        std::string("0.1000000000000000055511151231257827021181583404541015625"),
                        "0.1 to 100 digits: its exact value, which ends sooner");
    check::expect_equal(written(1e-300, 100),
                        std::string("1.00000000000000002505909183520875968569614680770370524992534231990046604318405148"
                                    "4676302812181950101e-300"),
                        "1e-300 to 100 digits");
}

// ---- Builders ----

// push() needs an array and set() an object: on any other type each throws std::logic_error, and leaves the value as it
// was. Json::object(keys, values) needs a value for each key, and builds the object in their order, for set() to go on.
void builders_on_the_wrong_type() {
    for (Json value : {Json(), Json(true), Json(5), Json("text"), Json::object(), parsed(R"({"a":1})")}) {
        const std::string before = value.dump();
        check::expect_throws<std::logic_error>([&] { value.push(1); }, "push() on " + before);
        check::expect_equal(value.dump(), before, "push() leaves " + before + " alone");
    }
    for (Json value : {Json(), Json(true), Json(5), Json("text"), Json::array(), parsed("[1]")}) {
        const std::string before = value.dump();
        check::expect_throws<std::logic_error>([&] { value.set("k", 1); }, "set() on " + before);
        check::expect_equal(value.dump(), before, "set() leaves " + before + " alone");
        check::expect(value.keys().empty(), "set() leaves " + before + " without keys");
    }

    const Json built = Json::object({"b", "a"}, {Json(1), Json("x")});
    check::expect(built.is_object() && keys_are(built, {"b", "a"}), "an object of the keys, in order");
    check::expect_equal(built.dump(), std::string(R"({"b":1,"a":"x"})"), "with their values");
    check::expect(Json::object({}, {}).is_object() && Json::object({}, {}).size() == 0, "an empty object");
    check::expect_throws<std::logic_error>([] { Json::object({"a", "b"}, {Json(1)}); }, "two keys and a value");
    check::expect_throws<std::logic_error>([] { Json::object({"a"}, {}); }, "a key without a value");
    check::expect_throws<std::logic_error>([] { Json::object({}, {Json(1)}); }, "a value without a key");
    Json extended = Json::object({"b", "a"}, {Json(1), Json(2)});
    extended.set("a", 3).set("c", 4);
    check::expect_equal(extended.dump(),
                        std::string(R"({"b":1,"a":3,"c":4})"),
                        "set() replaces in place, adds at the end");
}

// ---- Accessors ----

// Each type answers its own accessor; the others give neutral values, never throwing: false, 0, an empty string, no
// items, no keys, size 0, and nullptr from find(). An object's items are its values, and its keys come in order.
void accessors_on_the_wrong_type() {
    Json array = Json::array();
    array.push(1).push("two");
    Json object = Json::object();
    object.set("k", 1).set("j", "v");
    const Json values[] =
        {Json(), Json(true), Json(5), Json(2.5), Json("text"), Json(std::string("text")), array, object};
    for (const Json& value : values) {
        const std::string what = value.dump();
        if (!value.is_bool()) {
            check::expect(!value.as_bool(), what + ": as_bool() is false");
        }
        if (!value.is_number()) {
            check::expect_equal(value.as_number(), 0.0, what + ": as_number()");
        }
        if (!value.is_string()) {
            check::expect(value.as_string().empty(), what + ": as_string() is empty");
        }
        if (!value.is_array() && !value.is_object()) {
            check::expect(value.items().empty() && value.size() == 0, what + ": no items");
        }
        if (!value.is_object()) {
            check::expect(value.keys().empty(), what + ": no keys");
            check::expect(value.find("k") == nullptr && value.find("") == nullptr, what + ": find() finds nothing");
        }
    }
    check::expect(Json().is_null() && Json(true).is_bool() && Json(5).is_number() && Json(2.5).is_number() &&
                      Json("text").is_string() && Json(std::string("text")).is_string(),
                  "the constructors' types");
    check::expect(Json::array().is_array() && Json::object().is_object(), "array() and object()");
    check::expect_equal(Json(5).as_number(), 5.0, "an int is a number");
    check::expect(!Json(false).as_bool() && Json(true).as_bool(), "bools");
    check::expect_equal(array.size(), std::size_t{2}, "the array's size");
    check::expect(array.find("0") == nullptr, "find() on an array, even for an index");
    check::expect(keys_are(object, {"k", "j"}), "the object's keys, in order");
    check::expect(object.items()[0].as_number() == 1.0 && object.items()[1].as_string() == "v", "its values");
    check::expect(object.find("k") == &object.items()[0], "find() points into items()");
    check::expect(object.find("K") == nullptr && object.find("k ") == nullptr, "keys match exactly");
}

}  // namespace

int main() {
    return check::run({
        {"every type", every_type},
        {"whitespace is space, tab, line feed, and carriage return", whitespace},
        {"nesting, to 64 levels", nesting_and_its_limit},
        {"numbers: forms, rounding, -0, the extremes, and 1e400", numbers},
        {"numbers that underflow, and long ones, read alike on every platform", numbers_read_alike_everywhere},
        {"strings: escapes, \\u, surrogate pairs, and raw bytes", strings},
        {"malformed strings, and where their errors point", string_errors},
        {"other errors, and where they point", syntax_errors},
        {"a repeated key keeps its last value", duplicate_keys},
        {"200,000 keys parse in linear time, and repeats keep their first place", many_keys},
        {"round trips through dump", round_trips},
        {"dump writes numbers in the fewest characters", shortest_numbers},
        {"JsonWriter: nesting and fields", writer_nesting_and_fields},
        {"JsonWriter: escaping", writer_escaping},
        {"JsonWriter: numbers, with and without a digit limit", writer_numbers},
        {"JsonWriter: more digits than a double has", writer_many_digits},
        {"push() needs an array, set() an object, and object() a value per key", builders_on_the_wrong_type},
        {"accessors on values of the wrong type", accessors_on_the_wrong_type},
    });
}
