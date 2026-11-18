#include "stack_override.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>

namespace chipcae::importer {
std::string fold_layer_name(std::string name) {
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return name;
}
namespace {
// A schema-specific JSON reader keeps overrides available without an optional
// JSON dependency or a new ECAD dependency in non-ECAD builds. It accepts only
// the schema's object, array, string and finite-number productions.
class OverrideReader {
    std::string_view text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error(why + " at byte " + std::to_string(pos_));
    }
    void whitespace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                                       text_[pos_] == '\r' || text_[pos_] == '\t'))
            ++pos_;
    }
    bool take(char c) {
        whitespace();
        if (pos_ == text_.size() || text_[pos_] != c)
            return false;
        ++pos_;
        return true;
    }
    void expect(char c) {
        if (!take(c))
            fail(std::string("expected '") + c + "'");
    }
    unsigned hex4() {
        unsigned value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (pos_ == text_.size())
                fail("incomplete unicode escape");
            const char c = text_[pos_++];
            value *= 16;
            if (c >= '0' && c <= '9')
                value += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value += static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value += static_cast<unsigned>(c - 'A' + 10);
            else
                fail("invalid unicode escape");
        }
        return value;
    }
    void unicode(std::string& out) {
        unsigned code = hex4();
        if (code >= 0xd800 && code <= 0xdbff) {
            if (pos_ + 2 > text_.size() || text_.substr(pos_, 2) != "\\u")
                fail("missing low surrogate");
            pos_ += 2;
            const auto low = hex4();
            if (low < 0xdc00 || low > 0xdfff)
                fail("invalid low surrogate");
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff)
            fail("unexpected low surrogate");
        if (code < 0x80)
            out += static_cast<char>(code);
        else {
            if (code < 0x800)
                out += static_cast<char>(0xc0 | (code >> 6));
            else {
                if (code < 0x10000)
                    out += static_cast<char>(0xe0 | (code >> 12));
                else {
                    out += static_cast<char>(0xf0 | (code >> 18));
                    out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
                }
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            }
            out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    std::string string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const auto c = text_[pos_++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                fail("control character in string");
            if (c != '\\') {
                const auto byte = static_cast<unsigned char>(c);
                if (byte >= 0x80) {
                    unsigned code = 0, trailing = 0, minimum = 0;
                    if (byte >= 0xc2 && byte <= 0xdf) {
                        code = byte & 0x1f;
                        trailing = 1;
                        minimum = 0x80;
                    } else if (byte >= 0xe0 && byte <= 0xef) {
                        code = byte & 0x0f;
                        trailing = 2;
                        minimum = 0x800;
                    } else if (byte >= 0xf0 && byte <= 0xf4) {
                        code = byte & 0x07;
                        trailing = 3;
                        minimum = 0x10000;
                    } else
                        fail("invalid UTF-8 leading byte");
                    out += c;
                    for (unsigned i = 0; i < trailing; ++i) {
                        if (pos_ == text_.size())
                            fail("incomplete UTF-8 sequence");
                        const auto next = static_cast<unsigned char>(text_[pos_++]);
                        if ((next & 0xc0) != 0x80)
                            fail("invalid UTF-8 continuation byte");
                        code = (code << 6) | (next & 0x3f);
                        out += static_cast<char>(next);
                    }
                    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                        fail("invalid UTF-8 code point");
                } else
                    out += c;
                continue;
            }
            if (pos_ == text_.size())
                fail("incomplete escape");
            switch (text_[pos_++]) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u':
                unicode(out);
                break;
            default:
                fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }
    double number() {
        whitespace();
        const auto start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-')
            ++pos_;
        const auto digit = [&] {
            return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9';
        };
        if (!digit())
            fail("expected numeric thickness_m");
        if (text_[pos_] == '0')
            ++pos_;
        else
            while (digit())
                ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digit())
                fail("missing fractional digits");
            while (digit())
                ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (!digit())
                fail("missing exponent digits");
            while (digit())
                ++pos_;
        }
        double value = 0;
        const auto [end, error] = std::from_chars(text_.data() + start, text_.data() + pos_, value);
        if (error != std::errc{} || end != text_.data() + pos_ || !std::isfinite(value) ||
            value <= 0)
            fail("thickness_m must be finite and positive");
        return value;
    }
    template <class Member>
    void object(Member member) {
        expect('{');
        if (take('}'))
            return;
        std::set<std::string> keys;
        do {
            const auto key = string();
            if (!keys.insert(key).second)
                fail("duplicate JSON key: " + key);
            expect(':');
            member(key);
            if (take('}'))
                return;
            expect(',');
        } while (true);
    }
    void layers(ParsedStackOverride& result) {
        expect('[');
        if (take(']'))
            return;
        std::set<std::string> names;
        do {
            std::optional<std::string> name, material;
            std::optional<double> thickness;
            object([&](const std::string& key) {
                if (key == "name")
                    name = string();
                else if (key == "thickness_m")
                    thickness = number();
                else if (key == "material")
                    material = string();
                else
                    fail("unknown layer key: " + key);
            });
            if (!name || name->empty() || !thickness)
                fail("layer requires nonempty name and thickness_m");
            if (material && material->empty())
                fail("material ID must be nonempty");
            if (!names.insert(fold_layer_name(*name)).second)
                fail("duplicate layer name: " + *name);
            result.thicknesses.emplace(*name, *thickness);
            if (material)
                result.materials.emplace(*name, *material);
            if (take(']'))
                return;
            expect(',');
        } while (true);
    }

public:
    explicit OverrideReader(std::string_view text) : text_(text) {}
    ParsedStackOverride read() {
        ParsedStackOverride result;
        bool schema = false, has_layers = false;
        object([&](const std::string& key) {
            if (key == "schema") {
                if (string() != "chipcae.stack_override/0.1")
                    fail("wrong stack override schema");
                schema = true;
            } else if (key == "layers") {
                layers(result);
                has_layers = true;
            } else if (key == "source")
                result.source = string();
            else
                fail("unknown stack override key: " + key);
        });
        whitespace();
        if (pos_ != text_.size())
            fail("trailing JSON data");
        if (!schema || !has_layers)
            fail("schema and layers are required");
        return result;
    }
};
} // namespace
chipcae::geometry::Expected<ParsedStackOverride>
read_stack_override(const std::filesystem::path& file) {
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open stack override");
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        if (input.bad())
            throw std::runtime_error("cannot read stack override");
        return OverrideReader(text).read();
    } catch (const std::exception& error) {
        return chipcae::geometry::ImportThresholdError{"IMPORT_REJECTED: " +
                                                       std::string(error.what())};
    }
}
} // namespace chipcae::importer
