#include "chipcae/io/event_log.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace chipcae::io;
namespace {
void expect(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
Event make_event(std::uint64_t i) {
    Event e;
    e.seq = i;
    e.wall_time_unix_ns = 1000000 + static_cast<std::int64_t>(i);
    switch (i % 6) {
    case 0: {
        KernelHello hello;
        hello.run_id = "run-" + std::to_string(i);
        hello.attempt = static_cast<std::uint32_t>(i % 4);
        hello.protocols = {"run/v1", "control/v1"};
        hello.kernel_version = "v1";
        hello.schema_bundle_hash = "schema-hash";
        hello.plugins = {{"thermal", "2.3"}, {"flow", "1.0"}};
        hello.ranks = 4;
        hello.vtkhdf_version = {2, 9};
        hello.case_sha256 = "case-hash";
        hello.git_sha = "git-hash";
        hello.protocol_version = "1";
        hello.features = {"resume", "crc"};
        e.payload = hello;
        break;
    }
    case 1:
        e.payload = Progress{
            i, static_cast<double>(i) * .25, .25, static_cast<double>(i % 100) / 100.0, "step"};
        break;
    case 2:
        e.payload = Residual{
            "solver", "temperature", i, static_cast<std::uint32_t>(i % 20), 1.25, 3, "INTERNAL", 1};
        break;
    case 3:
        e.payload = LogLine{2, static_cast<std::int32_t>(i % 8), "message", "INTERNAL", 1};
        break;
    case 4:
        e.payload = State{State::Value::running, i, static_cast<double>(i)};
        break;
    default:
        e.payload = OutputAvailable{
            i, static_cast<double>(i), "out/" + std::to_string(i) + ".vtkhdf", "sha256"};
        break;
    }
    return e;
}
void same(const Event& a, const Event& b) {
    expect(a.seq == b.seq && a.wall_time_unix_ns == b.wall_time_unix_ns,
           "record metadata mismatch");
    expect(a.payload.index() == b.payload.index(), "payload kind mismatch");
    if (auto x = std::get_if<KernelHello>(&a.payload)) {
        auto y = std::get<KernelHello>(b.payload);
        expect(*x == y, "hello mismatch");
    }
    if (auto x = std::get_if<Progress>(&a.payload)) {
        auto y = std::get<Progress>(b.payload);
        expect(x->window == y.window && x->time == y.time && x->dt == y.dt &&
                   x->fraction == y.fraction && x->message == y.message,
               "progress mismatch");
    }
    if (auto x = std::get_if<Residual>(&a.payload)) {
        auto y = std::get<Residual>(b.payload);
        expect(x->solver == y.solver && x->field == y.field && x->window == y.window &&
                   x->iteration == y.iteration && x->norm == y.norm && x->flags == y.flags &&
                   x->code == y.code && x->error_code == y.error_code,
               "residual mismatch");
    }
    if (auto x = std::get_if<LogLine>(&a.payload)) {
        auto y = std::get<LogLine>(b.payload);
        expect(x->level == y.level && x->rank == y.rank && x->message == y.message &&
                   x->code == y.code && x->error_code == y.error_code,
               "logline mismatch");
    }
    if (auto x = std::get_if<State>(&a.payload)) {
        auto y = std::get<State>(b.payload);
        expect(x->value == y.value && x->window == y.window && x->time == y.time, "state mismatch");
    }
    if (auto x = std::get_if<OutputAvailable>(&a.payload)) {
        auto y = std::get<OutputAvailable>(b.payload);
        expect(x->window == y.window && x->time == y.time && x->path == y.path &&
                   x->checksum == y.checksum,
               "output mismatch");
    }
}
using Bytes = std::vector<unsigned char>;
Bytes load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
void save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}
std::uint32_t le32(const Bytes& bytes, std::size_t p) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes.at(p + i)) << (8 * i);
    return value;
}
// Independent bit-at-a-time CRC oracle for raw-format checks.
std::uint32_t checksum(const Bytes& bytes, std::size_t start, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = start; i < start + size; ++i) {
        crc ^= bytes.at(i);
        for (unsigned j = 0; j < 8; ++j) {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xedb88320u;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xffffffffu;
}
void store32(Bytes& bytes, std::size_t p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.at(p + i) = static_cast<unsigned char>(value >> (8 * i));
}
void regression_tests(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        for (std::uint64_t i = 1; i <= 6; ++i)
            writer.append(make_event(i));
    }
    const auto clean = load(path);
    std::vector<std::size_t> offsets{0};
    constexpr std::array<unsigned char, 6> kinds{10, 11, 20, 21, 22, 15};
    for (const auto kind : kinds) {
        const auto offset = offsets.back();
        const auto size = le32(clean, offset + 8);
        expect(clean.at(offset + 32) == kind, "kind must equal proto oneof field number");
        expect(le32(clean, offset + 12) == checksum(clean, offset + 16, size),
               "CRC must cover body and follow length");
        offsets.push_back(offset + 16 + size);
    }
    const Bytes known{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    expect(checksum(known, 0, known.size()) == 0xcbf43926u, "CRC standard vector");
    for (const auto lost : {0u, 1u, 5u, 12u}) {
        save(path, clean);
        std::filesystem::resize_file(path, clean.size() - lost);
        const std::uint64_t next = lost == 0 ? 7 : 6;
        {
            EventLogWriter writer(path);
            bool duplicate_rejected = false;
            try {
                writer.append(make_event(next - 1));
            } catch (const std::invalid_argument&) {
                duplicate_rejected = true;
            }
            expect(duplicate_rejected, "recovery restores last sequence");
            writer.append(make_event(next));
        }
        const auto events = EventLogReader(path).read_from(0);
        expect(events.size() == next, "crash recovery record count");
        for (std::size_t i = 0; i < events.size(); ++i)
            same(make_event(i + 1), events[i]);
    }
    // Recover partial magic, length, and CRC fields as well as partial bodies.
    for (const auto header_bytes : {0u, 1u, 8u, 11u, 12u, 15u}) {
        save(path, clean);
        std::filesystem::resize_file(path, offsets[5] + header_bytes);
        {
            EventLogWriter writer(path);
            writer.append(make_event(6));
        }
        const auto events = EventLogReader(path).read_from(0);
        expect(events.size() == 6, "partial header recovery");
        for (std::size_t i = 0; i < events.size(); ++i)
            same(make_event(i + 1), events[i]);
    }
    // Corruption in a complete record discards that record and everything after it.
    for (const auto flip : {offsets[3] + 12, offsets[3] + 16}) {
        auto corrupt = clean;
        corrupt.at(flip) ^= 1;
        save(path, corrupt);
        expect(EventLogReader(path).read_from(0).size() == 3, "CRC tail read");
        {
            EventLogWriter writer(path);
            expect(std::filesystem::file_size(path) == offsets[3], "CRC tail truncate");
            writer.append(make_event(4));
        }
        const auto events = EventLogReader(path).read_from(0);
        expect(events.size() == 4, "CRC recovery append");
        for (std::size_t i = 0; i < events.size(); ++i)
            same(make_event(i + 1), events[i]);
    }
    // An unknown kind with a matching CRC is a hard error.
    auto unknown = clean;
    unknown.at(32) = 99;
    store32(unknown, 12, checksum(unknown, 16, le32(unknown, 8)));
    save(path, unknown);
    bool rejected = false;
    try {
        (void)EventLogReader(path).read_from(0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "complete unknown kind must fail");
    save(path, clean);
    EventLogReader reader(path);
    std::uint64_t cursor = offsets[2];
    const auto mid = reader.read_from(3, &cursor);
    expect(mid.size() == 4 && cursor == clean.size(), "mid cursor resume");
    for (std::size_t i = 0; i < mid.size(); ++i)
        same(make_event(i + 3), mid[i]);
    expect(reader.read_from(7, &cursor).empty(), "poll at EOF");
    {
        EventLogWriter writer(path);
        writer.append(make_event(7));
        writer.append(make_event(8));
    }
    const auto polled = reader.read_from(7, &cursor);
    expect(polled.size() == 2, "two phase polling");
    same(make_event(7), polled[0]);
    same(make_event(8), polled[1]);
    expect(reader.read_from(9, &cursor).empty(), "no duplicate polling");
    cursor = 1;
    expect(reader.read_from(1, &cursor).size() == 8, "invalid cursor falls back");
    // A torn record must leave the cursor at its start for a later repaired append.
    save(path, clean);
    std::filesystem::resize_file(path, clean.size() - 5);
    EventLogReader torn_reader(path);
    cursor = 0;
    expect(torn_reader.read_from(1, &cursor).size() == 5 && cursor == offsets[5],
           "torn cursor boundary");
    {
        EventLogWriter writer(path);
        writer.append(make_event(6));
    }
    const auto repaired = torn_reader.read_from(6, &cursor);
    expect(repaired.size() == 1, "poll repaired tail");
    same(make_event(6), repaired.front());
    // Both directions enforce the frozen 64 MiB limit.
    auto oversized = clean;
    store32(oversized, 8, 64u * 1024u * 1024u + 1);
    save(path, oversized);
    rejected = false;
    try {
        (void)EventLogReader(path).read_from(0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "reader size limit");
    std::filesystem::remove(path);
    {
        EventLogWriter writer(path);
        auto large = make_event(3);
        std::get<LogLine>(large.payload).message.assign(64u * 1024u * 1024u, 'x');
        rejected = false;
        try {
            writer.append(large);
        } catch (const std::length_error&) {
            rejected = true;
        }
        expect(rejected && std::filesystem::file_size(path) == 0, "writer size limit");
        writer.append(make_event(1));
    }
    std::filesystem::remove(path);
}
} // namespace
int main() {
    try {
        const auto path = std::filesystem::temp_directory_path() / "chipcae-event-log-test.bin";
        std::filesystem::remove(path);
        {
            EventLogWriter writer(path);
            for (std::uint64_t i = 1; i <= 1000; ++i)
                writer.append(make_event(i));
            writer.close();
        }
        EventLogReader reader(path);
        const auto all = reader.read_from(1);
        expect(all.size() == 1000, "expected 1000 records");
        for (std::size_t i = 0; i < all.size(); ++i)
            same(make_event(i + 1), all[i]);
        for (const auto seq :
             {std::uint64_t{1}, std::uint64_t{500}, std::uint64_t{1000}, std::uint64_t{1001}}) {
            const auto suffix = reader.read_from(seq);
            expect(suffix.size() == (seq <= 1000 ? 1001 - seq : 0), "resume suffix length");
            if (!suffix.empty())
                expect(suffix.front().seq == seq, "resume is inclusive");
        }
        // Reopen and append verifies durable records and append semantics.
        {
            EventLogWriter writer(path);
            auto e = make_event(1001);
            writer.append(e);
        }
        expect(reader.read_from(1001).size() == 1, "reopen append");
        std::filesystem::remove(path);
        regression_tests(path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
