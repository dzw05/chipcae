#include "archive_codecs.hpp"

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstring>
#include <miniz.h>
#include <sstream>
#include <utility>

namespace chipcae::geometry::ecad::detail {
namespace {
// Buffered input keeps bytes read past the deflate end available for the trailer.
struct Input {
    std::istream& in;
    std::array<unsigned char, 32768> bytes{};
    std::size_t pos = 0, end = 0;
    std::uint64_t& consumed;
    bool fill() {
        if (pos != end)
            return true;
        in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        end = static_cast<std::size_t>(in.gcount());
        pos = 0;
        return end != 0;
    }
    int get() {
        if (!fill())
            return -1;
        ++consumed;
        return bytes[pos++];
    }
};
} // namespace
struct GzipCursor::State {
    struct promise_type {
        std::string_view block;
        State get_return_object() {
            return State(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(std::string_view value) noexcept {
            block = value;
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() { throw; }
    };
    std::coroutine_handle<promise_type> handle;
    ~State() {
        if (handle)
            handle.destroy();
    }
    State(State&& other) noexcept : handle(std::exchange(other.handle, {})) {}
    explicit State(std::coroutine_handle<promise_type> h) : handle(h) {}
    static State decode(std::istream& stream,
                        std::string& error,
                        std::uint64_t& consumed,
                        const std::size_t& output_limit) {
        Input input{stream, {}, 0, 0, consumed};
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
            if (header() != 31 || header() != 139 || header() != 8) {
                fail("invalid header");
                co_return;
            }
            int flags = header();
            if (flags < 0) {
                fail("truncated header");
                co_return;
            }
            if (flags & 0xe0) {
                fail("reserved flags");
                co_return;
            }
            for (int i = 0; i < 6; ++i)
                if (header() < 0) {
                    fail("truncated header");
                    co_return;
                }
            if (flags & 4) {
                int a = header(), b = header();
                if (a < 0 || b < 0) {
                    fail("truncated extra");
                    co_return;
                }
                for (int n = a + 256 * b; n; --n)
                    if (header() < 0) {
                        fail("truncated extra");
                        co_return;
                    }
            }
            for (int flag : {8, 16})
                if (flags & flag) {
                    int c;
                    do {
                        c = header();
                        if (c < 0) {
                            fail("truncated string");
                            co_return;
                        }
                    } while (c);
                }
            if (flags & 2) {
                int a = input.get(), b = input.get();
                if (a < 0 || b < 0) {
                    fail("truncated header CRC");
                    co_return;
                }
                if ((header_crc & 65535) != static_cast<unsigned>(a + b * 256)) {
                    fail("header CRC mismatch");
                    co_return;
                }
            }
            tinfl_decompressor inflater;
            tinfl_init(&inflater);
            std::array<mz_uint8, 2 * TINFL_LZ_DICT_SIZE> output{};
            std::size_t offset = 0;
            mz_ulong crc = MZ_CRC32_INIT;
            std::uint32_t size = 0;
            for (;;) {
                if (!input.fill()) {
                    fail("truncated deflate");
                    co_return;
                }
                // Near a caller's output boundary, expose only one compressed byte.
                // Otherwise tinfl can parse arbitrarily many empty deflate blocks
                // after filling the output span before it returns to us.
                std::size_t n = std::min<std::size_t>(input.end - input.pos,
                                                      output_limit <= 258 ? 1 : 32768),
                            count = std::min(output.size() - offset, output_limit);
                const bool more_input =
                    input.pos + n < input.end ||
                    (!stream.eof() && stream.peek() != std::char_traits<char>::eof());
                auto status = tinfl_decompress(&inflater,
                                               input.bytes.data() + input.pos,
                                               &n,
                                               output.data(),
                                               output.data() + offset,
                                               &count,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                                                   (more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0));
                input.pos += n;
                consumed += n;
                crc = mz_crc32(crc, output.data() + offset, count);
                size += static_cast<std::uint32_t>(count);
                if (count)
                    co_yield std::string_view{reinterpret_cast<char*>(output.data() + offset),
                                              count};
                offset += count;
                if (offset == output.size()) {
                    // Retain the full deflate history while allowing arbitrary output stops.
                    std::memmove(
                        output.data(), output.data() + TINFL_LZ_DICT_SIZE, TINFL_LZ_DICT_SIZE);
                    // tinfl may suspend halfway through a match. Its saved source
                    // offset must move with the retained history as well.
                    if (inflater.m_dist_from_out_buf_start >= TINFL_LZ_DICT_SIZE)
                        inflater.m_dist_from_out_buf_start -= TINFL_LZ_DICT_SIZE;
                    offset = TINFL_LZ_DICT_SIZE;
                }
                if (status == TINFL_STATUS_DONE)
                    break;
                if (status == TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS) {
                    fail("truncated deflate");
                    co_return;
                }
                if (status < 0) {
                    fail("invalid deflate");
                    co_return;
                }
                if (!n && !count) {
                    fail("truncated deflate");
                    co_return;
                }
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
            if (expected_crc == UINT64_MAX || expected_size == UINT64_MAX) {
                fail("truncated trailer");
                co_return;
            }
            if (expected_crc != crc) {
                fail("CRC mismatch");
                co_return;
            }
            if (expected_size != size) {
                fail("ISIZE mismatch");
                co_return;
            }
        }
        if (!member)
            fail("empty input");
    }
};
GzipCursor::GzipCursor(std::istream& stream)
    : state_(std::make_unique<State>(State::decode(stream, error_, consumed_, output_limit_))) {}
GzipCursor::~GzipCursor() = default;
bool GzipCursor::next(std::string_view& block, std::string& error, std::size_t max_output) {
    output_limit_ = std::max<std::size_t>(1, max_output);
    auto h = state_->handle;
    if (!h.done())
        h.resume();
    error = error_;
    if (h.done()) {
        block = {};
        return false;
    }
    block = h.promise().block;
    return true;
}
bool stream_gzip(std::istream& stream, const ByteSink& sink, std::string& error) {
    GzipCursor cursor(stream);
    std::string_view block;
    while (cursor.next(block, error))
        if (!sink(block))
            return false;
    return error.empty();
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
