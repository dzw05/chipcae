#include "chipcae/io/event_log.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chipcae::io {
namespace {
constexpr std::array<char, 8> magic{'C', 'A', 'E', 'L', 'O', 'G', '1', '\0'};
using Bytes = std::vector<std::uint8_t>;
template <class T>
void put(Bytes& b, T value) {
    static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>);
    std::uint64_t bits{};
    if constexpr (std::is_floating_point_v<T>) {
        if constexpr (sizeof(T) == 8)
            bits = std::bit_cast<std::uint64_t>(value);
        else
            bits = std::bit_cast<std::uint32_t>(value);
    } else
        bits = static_cast<std::make_unsigned_t<T>>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        b.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
}
template <class T>
T get(const Bytes& b, std::size_t& p) {
    if (b.size() - p < sizeof(T))
        throw std::runtime_error("truncated event record");
    std::uint64_t bits{};
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bits |= static_cast<std::uint64_t>(b[p++]) << (i * 8);
    if constexpr (std::is_floating_point_v<T>) {
        if constexpr (sizeof(T) == 8)
            return std::bit_cast<T>(bits);
        else
            return std::bit_cast<T>(static_cast<std::uint32_t>(bits));
    } else
        return static_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
}
void put_string(Bytes& b, const std::string& s) {
    if (s.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("event string too long");
    put(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
std::string get_string(const Bytes& b, std::size_t& p) {
    const auto n = get<std::uint32_t>(b, p);
    if (b.size() - p < n)
        throw std::runtime_error("truncated event string");
    std::string s(reinterpret_cast<const char*>(b.data() + p), n);
    p += n;
    return s;
}
Bytes encode(const Event& e) {
    Bytes b;
    put(b, e.seq);
    put(b, e.wall_time_unix_ns);
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, KernelHello>) {
                put(b, std::uint8_t{1});
                put_string(b, x.run_id);
                put(b, x.attempt);
                put_string(b, x.kernel_version);
            } else if constexpr (std::is_same_v<T, Progress>) {
                put(b, std::uint8_t{2});
                put(b, x.window);
                put(b, x.time);
                put(b, x.dt);
                put(b, x.fraction);
                put_string(b, x.message);
            } else if constexpr (std::is_same_v<T, Residual>) {
                put(b, std::uint8_t{3});
                put_string(b, x.solver);
                put_string(b, x.field);
                put(b, x.window);
                put(b, x.iteration);
                put(b, x.norm);
                put(b, x.flags);
                put_string(b, x.code);
                put(b, x.error_code);
            } else if constexpr (std::is_same_v<T, LogLine>) {
                put(b, std::uint8_t{4});
                put(b, x.level);
                put(b, x.rank);
                put_string(b, x.message);
                put_string(b, x.code);
                put(b, x.error_code);
            } else if constexpr (std::is_same_v<T, State>) {
                put(b, std::uint8_t{5});
                put(b, static_cast<std::uint8_t>(x.value));
                put(b, x.window);
                put(b, x.time);
            } else if constexpr (std::is_same_v<T, OutputAvailable>) {
                put(b, std::uint8_t{6});
                put(b, x.window);
                put(b, x.time);
                put_string(b, x.path);
                put_string(b, x.checksum);
            }
        },
        e.payload);
    return b;
}
Event decode(const Bytes& b) {
    std::size_t p = 0;
    Event e;
    e.seq = get<std::uint64_t>(b, p);
    e.wall_time_unix_ns = get<std::int64_t>(b, p);
    switch (get<std::uint8_t>(b, p)) {
    case 1: {
        KernelHello x;
        x.run_id = get_string(b, p);
        x.attempt = get<std::uint32_t>(b, p);
        x.kernel_version = get_string(b, p);
        e.payload = std::move(x);
        break;
    }
    case 2: {
        Progress x;
        x.window = get<std::uint64_t>(b, p);
        x.time = get<double>(b, p);
        x.dt = get<double>(b, p);
        x.fraction = get<double>(b, p);
        x.message = get_string(b, p);
        e.payload = std::move(x);
        break;
    }
    case 3: {
        Residual x;
        x.solver = get_string(b, p);
        x.field = get_string(b, p);
        x.window = get<std::uint64_t>(b, p);
        x.iteration = get<std::uint32_t>(b, p);
        x.norm = get<double>(b, p);
        x.flags = get<std::uint32_t>(b, p);
        x.code = get_string(b, p);
        x.error_code = get<std::uint32_t>(b, p);
        e.payload = std::move(x);
        break;
    }
    case 4: {
        LogLine x;
        x.level = get<std::int32_t>(b, p);
        x.rank = get<std::int32_t>(b, p);
        x.message = get_string(b, p);
        x.code = get_string(b, p);
        x.error_code = get<std::uint32_t>(b, p);
        e.payload = std::move(x);
        break;
    }
    case 5: {
        State x;
        const auto v = get<std::uint8_t>(b, p);
        if (v > 5)
            throw std::runtime_error("invalid state value");
        x.value = static_cast<State::Value>(v);
        x.window = get<std::uint64_t>(b, p);
        x.time = get<double>(b, p);
        e.payload = x;
        break;
    }
    case 6: {
        OutputAvailable x;
        x.window = get<std::uint64_t>(b, p);
        x.time = get<double>(b, p);
        x.path = get_string(b, p);
        x.checksum = get_string(b, p);
        e.payload = std::move(x);
        break;
    }
    default:
        throw std::runtime_error("unknown event payload kind");
    }
    if (p != b.size())
        throw std::runtime_error("event record has trailing data");
    return e;
}
} // namespace

class EventLogWriter::Impl {
public:
    explicit Impl(const std::filesystem::path& path)
        : stream(path, std::ios::binary | std::ios::app) {
        if (!stream)
            throw std::runtime_error("cannot open event log for append");
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 0) {
            const auto prior = EventLogReader(path).read_from(0);
            if (!prior.empty()) {
                last = prior.back().seq;
                has_last = true;
            }
        }
    }
    std::ofstream stream;
    std::uint64_t last{};
    bool has_last{};
};
EventLogWriter::EventLogWriter(const std::filesystem::path& p) : impl_(new Impl(p)) {}
EventLogWriter::~EventLogWriter() {
    try {
        close();
    } catch (...) {
    }
    delete impl_;
}
void EventLogWriter::append(const Event& event) {
    if (!impl_ || !impl_->stream)
        throw std::runtime_error("event log is closed");
    if (impl_->has_last && event.seq <= impl_->last)
        throw std::invalid_argument("event seq must increase");
    const auto b = encode(event);
    if (b.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("event record too large");
    impl_->stream.write(magic.data(), magic.size());
    Bytes n;
    put(n, static_cast<std::uint32_t>(b.size()));
    impl_->stream.write(reinterpret_cast<const char*>(n.data()), n.size());
    impl_->stream.write(reinterpret_cast<const char*>(b.data()),
                        static_cast<std::streamsize>(b.size()));
    impl_->stream.flush();
    if (!impl_->stream)
        throw std::runtime_error("failed writing event log");
    impl_->last = event.seq;
    impl_->has_last = true;
}
void EventLogWriter::close() {
    if (impl_ && impl_->stream.is_open()) {
        impl_->stream.flush();
        impl_->stream.close();
    }
}
EventLogReader::EventLogReader(std::filesystem::path p) : path_(std::move(p)) {}
std::vector<Event> EventLogReader::read_from(std::uint64_t seq) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        if (!std::filesystem::exists(path_))
            return {};
        throw std::runtime_error("cannot open event log");
    }
    std::vector<Event> result;
    while (true) {
        std::array<char, 8> m{};
        in.read(m.data(), m.size());
        if (in.gcount() == 0)
            break;
        if (in.gcount() != static_cast<std::streamsize>(m.size()))
            break;
        if (m != magic)
            throw std::runtime_error("invalid event log magic");
        std::array<std::uint8_t, 4> nb{};
        in.read(reinterpret_cast<char*>(nb.data()), nb.size());
        if (in.gcount() != 4)
            break;
        std::size_t q = 0;
        Bytes n(nb.begin(), nb.end());
        const auto size = get<std::uint32_t>(n, q);
        if (size > 64u * 1024u * 1024u)
            throw std::runtime_error("event record exceeds size limit");
        Bytes b(size);
        in.read(reinterpret_cast<char*>(b.data()), size);
        if (in.gcount() != static_cast<std::streamsize>(size))
            break;
        auto e = decode(b);
        if (e.seq >= seq)
            result.push_back(std::move(e));
    }
    return result;
}
} // namespace chipcae::io
