#include "chipcae/core/parse_floating.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <class T>
void decimal_cases() {
    struct Valid {
        std::string_view text;
        T expected;
    };
    for (const auto& [text, expected] : {Valid{"1", T{1}},
                                         Valid{"1.5", T{1.5}},
                                         Valid{"2e3", T{2000}},
                                         Valid{"-0.5", T{-0.5}},
                                         Valid{".5", T{0.5}},
                                         Valid{"5.", T{5}},
                                         Valid{"1E+2", T{100}},
                                         Valid{"0001.25", T{1.25}}}) {
        T value{};
        check(chipcae::parse_floating(text, value), text);
        check(std::abs(value - expected) <= std::numeric_limits<T>::epsilon(), text);
    }
    for (std::string_view text : {" 1",
                                  "+1",
                                  "1x",
                                  "",
                                  "1e",
                                  "0x10",
                                  "1e99999",
                                  "1e-99999",
                                  "-",
                                  ".",
                                  "1e+",
                                  "1 ",
                                  "1,5",
                                  "infinite",
                                  "nan(!)",
                                  "--1"}) {
        T value{7};
        check(!chipcae::parse_floating(text, value), text);
        check(std::abs(value - T{7}) <= std::numeric_limits<T>::epsilon(),
              "failed parse changed output");
    }
    T value{};
    check(!chipcae::parse_floating(std::string_view("1\0x", 3), value), "embedded NUL");
    for (std::string_view text : {"-0", "-0.0e99999"}) {
        check(chipcae::parse_floating(text, value), text);
        check(std::fpclassify(value) == FP_ZERO && std::signbit(value), "negative zero");
    }
    check(chipcae::parse_floating("0e-99999", value), "zero with extreme exponent");
    for (std::string_view text : {"inf", "INFINITY", "-iNf"}) {
        check(chipcae::parse_floating(text, value) && std::isinf(value), text);
        check(std::signbit(value) == text.starts_with('-'), "infinity sign");
    }
    for (std::string_view text : {"nan", "NaN()", "-NAN(payload_123)"}) {
        check(chipcae::parse_floating(text, value) && std::isnan(value), text);
        check(std::signbit(value) == text.starts_with('-'), "NaN sign");
    }
}

struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
};
} // namespace

int main() {
    try {
        decimal_cases<float>();
        decimal_cases<double>();
        decimal_cases<long double>();
        double value{};
        check(!chipcae::parse_floating("1e999", value), "double overflow");
        check(chipcae::parse_floating("1e-310", value) && std::fpclassify(value) == FP_SUBNORMAL,
              "representable subnormal");
        check(chipcae::parse_floating("5e-324", value) &&
                  std::abs(value - std::numeric_limits<double>::denorm_min()) <
                      std::numeric_limits<double>::denorm_min(),
              "smallest subnormal rounds correctly");
        check(!chipcae::parse_floating("1e-324", value), "nonzero underflow to zero");
        check(chipcae::parse_floating("2.2250738585072012e-308", value) &&
                  std::abs(value - std::numeric_limits<double>::min()) <
                      std::numeric_limits<double>::denorm_min(),
              "underflow rounding up to smallest normal");
        check(chipcae::parse_floating("1.7976931348623157e308", value) &&
                  std::bit_cast<std::uint64_t>(value) == UINT64_C(0x7fefffffffffffff),
              "largest finite double");
        check(!chipcae::parse_floating("1.7976931348623159e308", value), "double overflow edge");
        float single{};
        check(chipcae::parse_floating("1e-40", single) && std::fpclassify(single) == FP_SUBNORMAL,
              "float subnormal");
        check(!chipcae::parse_floating("1e-46", single), "float underflow to zero");
        check(chipcae::parse_floating("3.4028234663852886e38", single) &&
                  std::bit_cast<std::uint32_t>(single) == UINT32_C(0x7f7fffff),
              "largest finite float");
        check(!chipcae::parse_floating("3.4028236e38", single), "float overflow edge");
        check(chipcae::parse_floating("1.0000000000000002", value) &&
                  std::abs(value - std::nextafter(1.0, 2.0)) <
                      std::numeric_limits<double>::epsilon(),
              "round directly to target type");
        const auto previous = std::locale();
        std::locale::global(std::locale(previous, new CommaDecimal));
        decimal_cases<double>();
        std::locale::global(previous);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
