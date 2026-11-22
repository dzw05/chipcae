#pragma once

#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace chipcae {
// Parse a whole token using the floating from_chars(general) grammar. In particular,
// whitespace, a leading '+', hex numbers and trailing characters are not accepted.
// On failure the destination is unchanged. Callers retain their own finite/range checks.
template <class T>
bool parse_floating(std::string_view text, T& value) {
    static_assert(std::is_floating_point_v<T>);
    auto magnitude = text;
    const bool negative = magnitude.starts_with('-');
    if (negative)
        magnitude.remove_prefix(1);
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto matches = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char c =
                a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + ('a' - 'A')) : a[i];
            if (c != b[i])
                return false;
        }
        return true;
    };
    if (matches(magnitude, "inf") || matches(magnitude, "infinity")) {
        value = negative ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
        return true;
    }
    if (matches(magnitude.substr(0, 3), "nan")) {
        if (magnitude.size() != 3) {
            if (magnitude.size() < 5 || magnitude[3] != '(' || magnitude.back() != ')')
                return false;
            for (const char c : magnitude.substr(4, magnitude.size() - 5))
                if (!digit(c) && !alpha(c) && c != '_')
                    return false;
        }
        value = std::copysign(std::numeric_limits<T>::quiet_NaN(), negative ? T{-1} : T{1});
        return true;
    }

    std::size_t pos = 0;
    bool has_digits = false, nonzero = false;
    const auto digits = [&] {
        while (pos < magnitude.size() && digit(magnitude[pos])) {
            has_digits = true;
            nonzero = nonzero || magnitude[pos] != '0';
            ++pos;
        }
    };
    digits();
    if (pos < magnitude.size() && magnitude[pos] == '.') {
        ++pos;
        digits();
    }
    if (!has_digits)
        return false;
    if (pos < magnitude.size() && (magnitude[pos] == 'e' || magnitude[pos] == 'E')) {
        ++pos;
        if (pos < magnitude.size() && (magnitude[pos] == '+' || magnitude[pos] == '-'))
            ++pos;
        const auto start = pos;
        while (pos < magnitude.size() && digit(magnitude[pos]))
            ++pos;
        if (pos == start)
            return false;
    }
    if (pos != magnitude.size())
        return false;

    // Convert directly to T (no double rounding), independently of the process locale.
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    T parsed{};
    input >> std::noskipws >> parsed;
    // libstdc++ from_chars accepts representable double subnormals (e.g. 5e-324),
    // but reports result_out_of_range if a nonzero significand rounds to zero.
    // Streams do not consistently report that underflow, so check it explicitly.
    // libc++ may set failbit on ERANGE even when it returns a subnormal (or
    // rounds up to the smallest normal). Those values are still representable.
    const bool tiny =
        std::fpclassify(parsed) != FP_ZERO && std::abs(parsed) <= std::numeric_limits<T>::min();
    if (input.bad() || (input.fail() && !tiny) || !input.eof() || !std::isfinite(parsed) ||
        (nonzero && std::fpclassify(parsed) == FP_ZERO))
        return false;
    value = parsed;
    return true;
}
} // namespace chipcae
