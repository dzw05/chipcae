#include "archive.hpp"

#include <array>
#include <miniz.h>
#include <sstream>

namespace chipcae::geometry::ecad::detail {
namespace {
// Buffered input keeps bytes read past the deflate end available for the trailer.
struct Input {
    std::istream& in;
    std::array<unsigned char, 32768> bytes{};
    std::size_t pos = 0, end = 0;
    bool fill() {
        if (pos != end)
            return true;
        in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        end = static_cast<std::size_t>(in.gcount());
        pos = 0;
        return end != 0;
    }
    int get() { return fill() ? bytes[pos++] : -1; }
};
} // namespace
bool stream_gzip(std::istream& stream, const ByteSink& sink, std::string& error) {
    Input input{stream};
    auto fail = [&](const char* reason) {
        error = std::string("IMPORT_REJECTED: gzip ") + reason;
        return false;
    };
    bool member = false;
    while (input.fill()) {
        member = true;
        mz_ulong header_crc = MZ_CRC32_INIT;
        auto header = [&]() {
            int v = input.get();
            if (v >= 0) {
                auto b = static_cast<unsigned char>(v);
                header_crc = mz_crc32(header_crc, &b, 1);
            }
            return v;
        };
        if (header() != 31 || header() != 139 || header() != 8)
            return fail("invalid header");
        int flags = header();
        if (flags < 0)
            return fail("truncated header");
        if (flags & 0xe0)
            return fail("reserved flags");
        for (int i = 0; i < 6; ++i)
            if (header() < 0)
                return fail("truncated header");
        if (flags & 4) {
            int a = header(), b = header();
            if (a < 0 || b < 0)
                return fail("truncated extra");
            for (int n = a + 256 * b; n; --n)
                if (header() < 0)
                    return fail("truncated extra");
        }
        for (int flag : {8, 16})
            if (flags & flag) {
                int c;
                do {
                    c = header();
                    if (c < 0)
                        return fail("truncated string");
                } while (c);
            }
        if (flags & 2) {
            int a = input.get(), b = input.get();
            if (a < 0 || b < 0)
                return fail("truncated header CRC");
            if ((header_crc & 65535) != static_cast<unsigned>(a + b * 256))
                return fail("header CRC mismatch");
        }
        tinfl_decompressor inflater;
        tinfl_init(&inflater);
        std::array<mz_uint8, TINFL_LZ_DICT_SIZE> output{};
        std::size_t offset = 0;
        mz_ulong crc = MZ_CRC32_INIT;
        std::uint32_t size = 0;
        for (;;) {
            if (!input.fill())
                return fail("truncated deflate");
            std::size_t n = input.end - input.pos, count = output.size() - offset;
            auto status =
                tinfl_decompress(&inflater,
                                 input.bytes.data() + input.pos,
                                 &n,
                                 output.data(),
                                 output.data() + offset,
                                 &count,
                                 stream.eof() || stream.peek() == std::char_traits<char>::eof()
                                     ? 0
                                     : TINFL_FLAG_HAS_MORE_INPUT);
            input.pos += n;
            crc = mz_crc32(crc, output.data() + offset, count);
            size += static_cast<std::uint32_t>(count);
            if (count && !sink({reinterpret_cast<char*>(output.data() + offset), count}))
                return false;
            offset = (offset + count) % output.size();
            if (status == TINFL_STATUS_DONE)
                break;
            if (status == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS)
                return fail("truncated deflate");
            if (status < 0)
                return fail("invalid deflate");
            if (!n && !count)
                return fail("truncated deflate");
        }
        auto word = [&]() -> std::uint64_t {
            std::uint64_t v = 0;
            for (unsigned i = 0; i < 4; ++i) {
                int c = input.get();
                if (c < 0)
                    return UINT64_MAX;
                v |= static_cast<std::uint64_t>(c) << (i * 8);
            }
            return v;
        };
        auto expected_crc = word(), expected_size = word();
        if (expected_crc == UINT64_MAX || expected_size == UINT64_MAX)
            return fail("truncated trailer");
        if (expected_crc != crc)
            return fail("CRC mismatch");
        if (expected_size != size)
            return fail("ISIZE mismatch");
    }
    return member || fail("empty input");
}
bool decode_gzip(std::string_view input, std::string& output, std::string& error) {
    output.clear();
    error.clear();
    std::istringstream stream{std::string(input)};
    return stream_gzip(
        stream,
        [&](std::string_view block) {
            if (block.size() > ArchiveLimits{}.total_bytes - output.size()) {
                error = "IMPORT_REJECTED: total byte limit";
                return false;
            }
            output.append(block);
            return true;
        },
        error);
}
} // namespace chipcae::geometry::ecad::detail
