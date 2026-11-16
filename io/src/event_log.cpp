#include "chipcae/io/event_log.hpp"

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

namespace chipcae::io {
namespace {
constexpr std::array<char, 8> magic{'C', 'A', 'E', 'L', 'O', 'G', '1', '\0'};
constexpr std::uint32_t kMaxRecordBytes = 64u * 1024u * 1024u;
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
template <class T, class Write>
void put_list(Bytes& b, const std::vector<T>& values, Write write) {
    if (values.size() > kMaxRecordBytes / 4)
        throw std::length_error("event list too long");
    put(b, static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values)
        write(b, value);
}
template <class T, class Read>
std::vector<T> get_list(const Bytes& b, std::size_t& p, Read read) {
    const auto count = get<std::uint32_t>(b, p);
    if (count > (b.size() - p) / 4)
        throw std::runtime_error("truncated event list");
    std::vector<T> values;
    for (std::uint32_t i = 0; i < count; ++i)
        values.push_back(read(b, p));
    return values;
}
void put_plugin(Bytes& b, const Plugin& plugin) {
    put_string(b, plugin.key);
    put_string(b, plugin.version);
}
Plugin get_plugin(const Bytes& b, std::size_t& p) {
    Plugin plugin;
    plugin.key = get_string(b, p);
    plugin.version = get_string(b, p);
    return plugin;
}
std::uint32_t crc32(const Bytes& b) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : b) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}
Bytes encode(const Event& e) {
    Bytes b;
    put(b, e.seq);
    put(b, e.wall_time_unix_ns);
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, KernelHello>) {
                put(b, std::uint8_t{15});
                put_string(b, x.run_id);
                put(b, x.attempt);
                put_list(b, x.protocols, put_string);
                put_string(b, x.kernel_version);
                put_string(b, x.schema_bundle_hash);
                put_list(b, x.plugins, put_plugin);
                put(b, x.ranks);
                put(b, x.event_log_format);
                put_list(b, x.vtkhdf_version, put<std::uint32_t>);
                put_string(b, x.case_sha256);
                put_string(b, x.git_sha);
                put_string(b, x.protocol_version);
                put_list(b, x.features, put_string);
            } else if constexpr (std::is_same_v<T, Progress>) {
                put(b, std::uint8_t{10});
                put(b, x.window);
                put(b, x.time);
                put(b, x.dt);
                put(b, x.fraction);
                put_string(b, x.message);
            } else if constexpr (std::is_same_v<T, Residual>) {
                put(b, std::uint8_t{11});
                put_string(b, x.solver);
                put_string(b, x.field);
                put(b, x.window);
                put(b, x.iteration);
                put(b, x.norm);
                put(b, x.flags);
                put_string(b, x.code);
                put(b, x.error_code);
            } else if constexpr (std::is_same_v<T, LogLine>) {
                put(b, std::uint8_t{20});
                put(b, x.level);
                put(b, x.rank);
                put_string(b, x.message);
                put_string(b, x.code);
                put(b, x.error_code);
            } else if constexpr (std::is_same_v<T, State>) {
                put(b, std::uint8_t{21});
                put(b, static_cast<std::uint8_t>(x.value));
                put(b, x.window);
                put(b, x.time);
            } else if constexpr (std::is_same_v<T, OutputAvailable>) {
                put(b, std::uint8_t{22});
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
    case 15: {
        KernelHello x;
        x.run_id = get_string(b, p);
        x.attempt = get<std::uint32_t>(b, p);
        x.protocols = get_list<std::string>(b, p, get_string);
        x.kernel_version = get_string(b, p);
        x.schema_bundle_hash = get_string(b, p);
        x.plugins = get_list<Plugin>(b, p, get_plugin);
        x.ranks = get<std::uint32_t>(b, p);
        x.event_log_format = get<std::uint32_t>(b, p);
        x.vtkhdf_version = get_list<std::uint32_t>(b, p, get<std::uint32_t>);
        x.case_sha256 = get_string(b, p);
        x.git_sha = get_string(b, p);
        x.protocol_version = get_string(b, p);
        x.features = get_list<std::string>(b, p, get_string);
        e.payload = std::move(x);
        break;
    }
    case 10: {
        Progress x;
        x.window = get<std::uint64_t>(b, p);
        x.time = get<double>(b, p);
        x.dt = get<double>(b, p);
        x.fraction = get<double>(b, p);
        x.message = get_string(b, p);
        e.payload = std::move(x);
        break;
    }
    case 11: {
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
    case 20: {
        LogLine x;
        x.level = get<std::int32_t>(b, p);
        x.rank = get<std::int32_t>(b, p);
        x.message = get_string(b, p);
        x.code = get_string(b, p);
        x.error_code = get<std::uint32_t>(b, p);
        e.payload = std::move(x);
        break;
    }
    case 21: {
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
    case 22: {
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
// Advance offset only after framing, CRC, and payload validation succeed.
bool read_record(std::ifstream& in, Event& event, std::uint64_t& offset) {
    std::array<char, 8> m{};
    in.read(m.data(), m.size());
    if (in.gcount() != static_cast<std::streamsize>(m.size()))
        return false;
    if (m != magic)
        throw std::runtime_error("invalid event log magic");
    Bytes header(8);
    in.read(reinterpret_cast<char*>(header.data()), header.size());
    if (in.gcount() != 8)
        return false;
    std::size_t p = 0;
    const auto size = get<std::uint32_t>(header, p);
    const auto expected_crc = get<std::uint32_t>(header, p);
    if (size > kMaxRecordBytes)
        return false; // oversized length is a dropped tail, not a hard error
    Bytes body(size);
    in.read(reinterpret_cast<char*>(body.data()), size);
    if (in.gcount() != static_cast<std::streamsize>(size) || crc32(body) != expected_crc)
        return false;
    event = decode(body);
    offset += 16 + size;
    return true;
}
std::filesystem::path salvage_path(const std::filesystem::path& path, std::uint64_t valid_end) {
    const auto base = path.string() + ".corrupt-" + std::to_string(valid_end);
    if (!std::filesystem::exists(base))
        return base;
    for (unsigned n = 1; n < 10000; ++n) {
        const auto candidate = base + "-" + std::to_string(n);
        if (!std::filesystem::exists(candidate))
            return candidate;
    }
    throw std::runtime_error("too many event log salvage files at this offset");
}

} // namespace

class EventLogWriter::Impl {
public:
    explicit Impl(const std::filesystem::path& path) {
        if (std::filesystem::exists(path)) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw std::runtime_error("cannot open event log for recovery");
            std::uint64_t valid_end = 0;
            Event event;
            while (read_record(in, event, valid_end)) {
                last = event.seq;
                has_last = true;
            }
            in.close();
            const auto file_size = std::filesystem::file_size(path);
            if (file_size > valid_end) {
                const auto salvage = salvage_path(path, valid_end);
                {
                    std::ifstream src(path, std::ios::binary);
                    src.seekg(static_cast<std::streamoff>(valid_end));
                    std::ofstream dst(salvage, std::ios::binary);
                    if (!src || !dst || !(dst << src.rdbuf()))
                        throw std::runtime_error("cannot salvage event log tail");
                }
                static std::once_flag warn;
                std::call_once(warn, [&] {
                    std::cerr << "warning: event log truncated; dropped tail saved to " << salvage
                              << "\n";
                });
            }
            std::filesystem::resize_file(path, valid_end);
        }
        stream.open(path, std::ios::binary | std::ios::app);
        if (!stream)
            throw std::runtime_error("cannot open event log for append");
    }
    std::ofstream stream;
    std::uint64_t last{};
    bool has_last{};
};
EventLogWriter::EventLogWriter(const std::filesystem::path& p) : impl_(std::make_unique<Impl>(p)) {}
EventLogWriter::~EventLogWriter() {
    try {
        close();
    } catch (...) {
    }
}
void EventLogWriter::append(const Event& event) {
    if (!impl_ || !impl_->stream.is_open() || !impl_->stream)
        throw std::runtime_error("event log is closed");
    if (impl_->has_last && event.seq <= impl_->last)
        throw std::invalid_argument("event seq must increase");
    const auto b = encode(event);
    if (b.size() > kMaxRecordBytes)
        throw std::length_error("event record too large");
    impl_->stream.write(magic.data(), magic.size());
    Bytes n;
    put(n, static_cast<std::uint32_t>(b.size()));
    put(n, crc32(b));
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
std::vector<Event> EventLogReader::read_from(std::uint64_t seq, std::uint64_t* next_offset) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        if (!std::filesystem::exists(path_)) {
            if (next_offset)
                *next_offset = 0;
            return {};
        }
        throw std::runtime_error("cannot open event log");
    }
    const auto file_size = std::filesystem::file_size(path_);
    if (!boundaries_.empty() && file_size < *boundaries_.rbegin())
        boundaries_ = {0};
    std::uint64_t offset = 0;
    const auto requested = next_offset ? *next_offset : 0;
    if (requested != 0 && !boundaries_.contains(requested)) {
        Event event;
        while (offset < requested && read_record(in, event, offset))
            boundaries_.insert(offset);
        in.clear();
    }
    offset = boundaries_.contains(requested) && requested <= std::filesystem::file_size(path_)
                 ? requested
                 : 0;
    in.seekg(static_cast<std::streamoff>(offset));
    std::vector<Event> result;
    Event event;
    while (read_record(in, event, offset)) {
        boundaries_.insert(offset);
        if (event.seq >= seq)
            result.push_back(std::move(event));
    }
    if (next_offset)
        *next_offset = offset;
    return result;
}
} // namespace chipcae::io
