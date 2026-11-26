#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "launch_support.hpp"
#include "run_dir.hpp"
#include "sha256_stream.hpp"

#include <array>
#include <cctype>
#include <fcntl.h>
#include <limits>
#include <random>
#include <set>
#include <spawn.h>
#include <sys/resource.h>

extern char** environ;
using namespace jobmgr_test;

namespace {
constexpr std::size_t mib = std::size_t{1} << 20;

const char* binary_digest(std::uintmax_t size) {
    return size == mib ? "357392daf5a4290dcc8f0d0985fde9bce947fbb2ef5ec8a442600e664e22256a"
                       : "847c0e04339e4e3aab14b64dfae13e71cffd9ee04fd26366f61b87f63472201c";
}

// Fixed-size generation and comparison keep the parent independent of attachment size.
void generate_binary(const fs::path& path, std::size_t size) {
    std::uint32_t random = 0x51a256U;
    std::array<char, 65536> buffer{};
    std::ofstream out(path, std::ios::binary);
    for (auto& byte : buffer) {
        random = random * 1664525U + 1013904223U;
        byte = static_cast<char>(random >> 24);
    }
    buffer[0] = '\0';
    while (size != 0) {
        const auto count = std::min(size, buffer.size());
        out.write(buffer.data(), static_cast<std::streamsize>(count));
        size -= count;
    }
    out.close();
    require(!out.fail(), "generate binary attachment");
}

void same_file(const fs::path& source, const fs::path& target) {
    require(fs::file_size(source) == fs::file_size(target), "CLI snapshot size differs");
    std::ifstream a(source, std::ios::binary), b(target, std::ios::binary);
    std::array<char, 65536> left{}, right{};
    while (a) {
        a.read(left.data(), static_cast<std::streamsize>(left.size()));
        b.read(right.data(), static_cast<std::streamsize>(right.size()));
        require(a.gcount() == b.gcount() &&
                    std::equal(left.begin(), left.begin() + a.gcount(), right.begin()),
                "CLI snapshot bytes differ");
    }
    require(a.eof() && b.eof(), "CLI snapshot read error");
}

long submit_rss(Fixture& f, const fs::path& root) {
    std::vector<std::string> args{CHIPCAE_JOBMGR_CLI,
                                  "--root",
                                  root.string(),
                                  "submit",
                                  "--case",
                                  f.case_path.string(),
                                  "--profile",
                                  f.profile_path.string(),
                                  "--request-id",
                                  f.id(),
                                  "--solver",
                                  "/bin/true",
                                  "--mpi",
                                  "/bin/true"};
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    ChildCleanup child;
    require(posix_spawn(&child.pid, argv[0], nullptr, nullptr, argv.data(), environ) == 0,
            "posix_spawn CLI failed");
    int status = 0;
    struct rusage usage{};
    pid_t waited;
    do {
        waited = wait4(child.pid, &status, 0, &usage);
    } while (waited < 0 && errno == EINTR);
    require(waited == child.pid, "wait4 CLI failed");
    child.pid = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "CLI submit failed");
    require(directory_count(root) == 1, "CLI must create exactly one run");
    fs::path run;
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.is_directory())
            run = entry.path();
    const auto job = parse_pb<jobs::Job>(run / "job.pb");
    require(job.state() == jobs::JOB_STATE_RUNNING,
            "CLI Submit must succeed: " + job.failure().message());
    same_file(f.source / "board.bin", run / "inputs/board.bin");
    const auto manifest = Json::parse(read(run / "snapshot-hashes.json"));
    // Independent Python hashlib digests of the fixed seeded 64 KiB block repeated to size.
    const auto expected = binary_digest(fs::file_size(f.source / "board.bin"));
    require(manifest.at((run / "inputs/board.bin").string()).as<std::string>() == expected,
            "CLI attachment digest differs from independent fixture digest");
#if defined(__APPLE__)
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}

void memory_regression() {
    std::array<long, 2> minimum{std::numeric_limits<long>::max(), std::numeric_limits<long>::max()};
    for (std::size_t index = 0; index < minimum.size(); ++index) {
        Fixture f;
        const auto size = (index == 0 ? 1 : 48) * mib;
        generate_binary(f.source / "board.bin", size);
        auto stack = Json::parse(read(f.source / "fcbga_demo.stack.json"));
        Json attachment = Json::object();
        attachment["path"] = "board.bin";
        attachment["sha256"] = std::string(binary_digest(size));
        stack["external_refs"] = Json::array();
        stack["external_refs"].push_back(attachment);
        write(f.source / "fcbga_demo.stack.json", stack.to_string());
        for (unsigned repetition = 0; repetition < 3; ++repetition) {
            const auto rss = submit_rss(f, f.temp.path / ("runs-" + std::to_string(repetition)));
            minimum[index] = std::min(minimum[index], rss);
            std::cout << "RSS size_MiB=" << size / mib << " repetition=" << repetition
                      << " KiB=" << rss << '\n';
        }
    }
    const auto delta = minimum[1] - minimum[0];
    std::cout << "minRSS(48)-minRSS(1)=" << delta << " KiB\n";
    require(delta <= 4096, "binary snapshot RSS growth exceeds 4096 KiB");
}
} // namespace

namespace {
using chipcae::jobmgr::sha256_hex;
using chipcae::jobmgr::detail::Sha256;
using chipcae::jobmgr::detail::snapshot;
using chipcae::jobmgr::detail::stream_copy_hash;

std::set<fs::path> entries(const fs::path& root) {
    std::set<fs::path> result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        result.insert(entry.path());
    return result;
}
void no_temporaries(const fs::path& root) {
    for (const auto& entry : entries(root))
        require(entry.filename().string().find(".tmp-") == std::string::npos,
                "temporary output leaked: " + entry.string());
}
std::string seeded(std::size_t size) {
    std::mt19937 random(0x51a256U);
    std::string result(size, '\0');
    for (auto& c : result)
        c = static_cast<char>(random() & 255U);
    return result;
}
void correctness_tests(Suite& suite) {
    const std::vector<std::pair<std::string, std::string>> vectors{
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for (const auto& [input, expected] : vectors) {
        suite.test("one-shot FIPS SHA size=" + std::to_string(input.size()), [&] {
            require(sha256_hex(input) == expected, "one-shot standard digest differs");
        });
        suite.test("incremental FIPS SHA size=" + std::to_string(input.size()), [&] {
            Sha256 hash;
            // Multi-update million-a also exercises repeated complete blocks.
            for (std::size_t i = 0; i < input.size(); i += 37)
                hash.update(std::string_view(input).substr(i, 37));
            require(hash.hex_final() == expected, "incremental standard digest differs");
        });
    }
    suite.test("incremental SHA every split including padding boundaries", [] {
        // Includes 55/56/63/64/65/119/120/128 and every other length through 200.
        for (std::size_t n = 0; n <= 200; ++n) {
            const auto input = seeded(n);
            const auto expected = sha256_hex(input);
            for (std::size_t k = 0; k <= n; ++k) {
                Sha256 hash;
                hash.update(nullptr, 0);
                hash.update(input.data(), k);
                hash.update(std::string_view(input).substr(k));
                require(hash.hex_final() == expected,
                        "split mismatch n=" + std::to_string(n) + " k=" + std::to_string(k));
            }
        }
    });
    for (const auto size : {std::size_t{0}, mib - 1, mib, mib + 1, 2 * mib}) {
        suite.test("stream copy and hash-only boundary=" + std::to_string(size), [=] {
            TempDirectory temp;
            const auto source = temp.path / "source";
            const auto target = temp.path / "new/target";
            const auto bytes = seeded(size);
            write(source, bytes);
            const auto expected = sha256_hex(bytes);
            const auto copied = stream_copy_hash(source, target, expected);
            require(copied.sha256 == expected, "streamed SHA differs from source SHA");
            require(copied.bytes == size, "stream byte count differs");
            require(read(target) == bytes, "stream copy changed source bytes");
            const auto before = entries(temp.path);
            const auto hashed = stream_copy_hash(source, {}, expected);
            require(hashed.sha256 == copied.sha256 && hashed.bytes == copied.bytes &&
                        hashed.json_candidate == copied.json_candidate,
                    "hash-only metadata differs");
            require(entries(temp.path) == before, "hash-only call created filesystem entries");
            no_temporaries(temp.path);
        });
    }
    const std::vector<std::pair<std::string, bool>> candidates{
        {"{", true},
        {"  \t\r\n[", true},
        {"\xef\xbb\xbf{", true},
        {"/* c */{", true},
        {"// c\n{", true},
        {std::string(mib + 3, ' ') + "{}", true},
        {"\"str\"", false},
        {"123", false},
        {std::string("\0{", 2), false},
        {"", false},
        {" \t\r\n", false},
        {"\xef", false},
        {"\xef\xbb", false},
        {"\xef{", false},
        {"\xef\xbb{", false},
        {std::string("\xef\xbb\x00{", 4), false},
        {"\v{", false},
        {"\f{", false}};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        suite.test("JSON candidate prefix=" + std::to_string(i), [&] {
            TempDirectory temp;
            const auto& [bytes, expected] = candidates[i];
            write(temp.path / "source", bytes);
            const auto copied = stream_copy_hash(temp.path / "source", temp.path / "copy", "");
            require(copied.json_candidate == expected, "wrong JSON candidate classification");
            require(fs::is_regular_file(temp.path / "copy"), "stream did not publish target");
            require(read(temp.path / "copy") == bytes, "candidate/malformed bytes changed");
            const auto before = entries(temp.path);
            const auto hashed = stream_copy_hash(temp.path / "source", {}, "");
            require(hashed.json_candidate == expected && hashed.sha256 == copied.sha256 &&
                        hashed.bytes == bytes.size(),
                    "hash-only candidate metadata differs");
            require(entries(temp.path) == before, "hash-only candidate created entries");
        });
    }
}

void io_tests(Suite& suite) {
    suite.test("hash mismatch removes new output and preserves existing output", [] {
        TempDirectory temp;
        const auto source = temp.path / "source";
        const auto target = temp.path / "output";
        write(source, "abc");
        const std::string wrong(64, '0');
        throws_message([&] { stream_copy_hash(source, target, wrong); }, "INPUT_HASH_MISMATCH");
        require(!fs::exists(target), "hash mismatch published a new target");
        no_temporaries(temp.path);
        write(target, "sentinel");
        throws_message([&] { stream_copy_hash(source, target, wrong); }, "INPUT_HASH_MISMATCH");
        require(read(target) == "sentinel", "mismatch overwrote an existing target");
        const auto before = entries(temp.path);
        throws_message([&] { stream_copy_hash(source, {}, wrong); }, "INPUT_HASH_MISMATCH");
        require(entries(temp.path) == before, "hash-only mismatch created entries");
        no_temporaries(temp.path);
    });
    suite.test("missing source fails before creating output parents or temporary files", [] {
        TempDirectory temp;
        const auto missing = temp.path / "missing";
        const auto target = temp.path / "new/output";
        const auto before = entries(temp.path);
        throws_message([&] { stream_copy_hash(missing, target, ""); },
                       "Cannot read " + missing.string());
        require(entries(temp.path) == before, "missing input created output entries");
        throws_message([&] { stream_copy_hash(missing, {}, ""); },
                       "Cannot read " + missing.string());
        no_temporaries(temp.path);
    });
    suite.test("source read error is not EOF and cleans temporary output", [] {
        TempDirectory temp;
        const auto source = temp.path / "directory-source";
        fs::create_directory(source);
        const auto target = temp.path / "output";
        throws_message([&] { stream_copy_hash(source, target, ""); },
                       "Cannot read " + source.string());
        require(!fs::exists(target), "read error published target");
        no_temporaries(temp.path);
    });
    suite.test("unwritable destination cleans temporary files", [] {
        if (geteuid() == 0) {
            std::cout << "SKIP subcase: chmod-0555 destination (root bypasses permissions)\n";
            return;
        }
        TempDirectory temp;
        const auto parent = temp.path / "readonly";
        fs::create_directory(parent);
        write(temp.path / "source", "abc");
        struct RestorePermissions {
            fs::path path;
            fs::perms original;
            ~RestorePermissions() {
                std::error_code ignored;
                fs::permissions(path, original, ignored);
            }
        } restore{parent, fs::status(parent).permissions()};
        fs::permissions(parent,
                        fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                            fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec);
        bool threw = false;
        try {
            stream_copy_hash(temp.path / "source", parent / "output", "");
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "unwritable destination accepted output");
        require(!fs::exists(parent / "output"), "write failure published target");
        no_temporaries(temp.path);
    });
}

void snapshot_tests(Suite& suite) {
    suite.test("Submit follows BOM and comment refs with binary bytes and exact I7 layout", [] {
        Fixture f;
        write(f.source / "board.bin", seeded(mib + 1));
        write(f.source / "bom.json", "\xef\xbb\xbf{\"ref\":\"block.json\"}");
        write(f.source / "block.json", "/* c */{\"ref\":\"line.json\"}");
        write(f.source / "line.json", "// c\n{\"ref\":\"./bom.json\"}");
        write(f.source / "malformed.json", "{");
        auto stack = Json::parse(read(f.source / "fcbga_demo.stack.json"));
        stack["external_refs"] = Json::array();
        for (const auto name : {"board.bin", "bom.json", "./bom.json", "malformed.json"}) {
            Json attachment = Json::object();
            attachment["path"] = name;
            attachment["sha256"] = sha256_hex(read(f.source / name));
            stack["external_refs"].push_back(attachment);
        }
        auto case_value = f.case_json();
        case_value["x-ignored"] = Json::parse(R"({"ref":"missing-ignored"})");
        f.case_json(case_value);
        write(f.source / "fcbga_demo.stack.json", stack.to_string());
        auto request = f.request();
        auto upper = sha256_hex(read(f.case_path));
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        request.mutable_case_file()->set_sha256(upper);
        auto manager = f.manager();
        const auto job = manager->Submit(request, f.case_path, f.profile_path);
        require(job.state() == jobs::JOB_STATE_RUNNING,
                "stream snapshot Submit failed: " + job.failure().message());
        const fs::path run(job.result_dir());
        for (const auto file : {"job.pb",
                                "submit.pb",
                                "control.jsonl",
                                "run.json",
                                "inputs/case.json",
                                "snapshot-hashes.json"})
            require(fs::is_regular_file(run / file), std::string("missing I7 file ") + file);
        require(read(run / "control.jsonl").empty(), "new control journal is nonempty");
        require(read(run / "submit.pb") == request.SerializeAsString(), "submit bytes changed");
        const auto hashes = Json::parse(read(run / "snapshot-hashes.json"));
        const std::vector<std::pair<fs::path, fs::path>> files{
            {f.case_path, run / "inputs/case.json"},
            {f.profile_path, run / "run.json"},
            {f.source / "fcbga_demo.stack.json", run / "inputs/fcbga_demo.stack.json"},
            {f.source / "fcbga_demo.materials.json", run / "inputs/fcbga_demo.materials.json"},
            {f.source / "board.bin", run / "inputs/board.bin"},
            {f.source / "bom.json", run / "inputs/bom.json"},
            {f.source / "block.json", run / "inputs/block.json"},
            {f.source / "line.json", run / "inputs/line.json"},
            {f.source / "malformed.json", run / "inputs/malformed.json"}};
        require(hashes.size() == files.size(), "duplicate/cyclic target has multiple hash entries");
        for (const auto& [source, target] : files) {
            require(read(source) == read(target), "snapshot bytes differ from source");
            require(hashes.at(target.lexically_normal().string()).as<std::string>() ==
                        sha256_hex(read(source)),
                    "manifest digest differs from source");
        }
        require(job.case_sha256() == sha256_hex(read(run / "inputs/case.json")),
                "case SHA differs from snapshot SHA");
        persisted(*manager, job);
        no_temporaries(run);
    });
    suite.test("visited normalized destination verifies source without rewriting sentinel", [] {
        TempDirectory temp;
        const auto source = temp.path / "source";
        const auto target = temp.path / "output";
        write(source, "abc");
        Json hashes = Json::object();
        std::set<fs::path> visited;
        snapshot(source, target, hashes, visited);
        write(target, "sentinel");
        auto upper = sha256_hex("abc");
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        snapshot(source, temp.path / "./output", hashes, visited, upper);
        require(read(target) == "sentinel", "duplicate snapshot rewrote target");
        require(visited.size() == 1 && hashes.size() == 1, "normalized target recorded twice");
        require(hashes.at(target.string()).as<std::string>() == sha256_hex("abc"),
                "duplicate snapshot changed original hash");
        throws_message([&] { snapshot(source, target, hashes, visited, std::string(64, '0')); },
                       "INPUT_HASH_MISMATCH");
        fs::remove(source);
        throws_message([&] { snapshot(source, target, hashes, visited); },
                       "Cannot read " + source.string());
        require(read(target) == "sentinel", "failed duplicate rewrote target");
        no_temporaries(temp.path);
    });
    suite.test("conflicting second reference maps to FAILED CASE_INVALID exact message", [] {
        Fixture f;
        write(f.source / "board.bin", "abc");
        auto stack = Json::parse(read(f.source / "fcbga_demo.stack.json"));
        auto refs = Json::array();
        Json first = Json::object();
        first["path"] = "board.bin";
        first["sha256"] = sha256_hex("abc");
        refs.push_back(first);
        first["path"] = "./board.bin";
        first["sha256"] = std::string(64, '0');
        refs.push_back(first);
        stack["external_refs"] = refs;
        write(f.source / "fcbga_demo.stack.json", stack.to_string());
        auto manager = f.manager();
        const auto job = f.submit(*manager);
        failure(job, jobs::FAILURE_CATEGORY_CASE_INVALID, "INPUT_HASH_MISMATCH");
        require(read(fs::path(job.result_dir()) / "inputs/board.bin") == "abc",
                "conflicting duplicate changed first snapshot");
        require(f.fake->starts.empty(), "invalid snapshot launched a process");
        no_temporaries(job.result_dir());
    });
}
} // namespace

namespace {
void failed_snapshot_tests(Suite& suite) {
    suite.test("failed snapshot does not mark unwritten destination visited", [] {
        TempDirectory temp;
        write(temp.path / "source", "abc");
        write(temp.path / "parent-is-file", "blocker");
        const auto target = temp.path / "parent-is-file/output";
        Json hashes = Json::object();
        std::set<fs::path> visited;
        bool threw = false;
        try {
            snapshot(temp.path / "source", target, hashes, visited);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, "impossible destination accepted");
        require(visited.empty(), "failed output was marked visited before successful copy");
        require(hashes.empty(), "failed output was recorded in hashes");
        no_temporaries(temp.path);
    });
}
} // namespace

int main() {
    Suite suite;
    suite.test("CLI binary snapshot memory is independent of input size", memory_regression);
    correctness_tests(suite);
    io_tests(suite);
    snapshot_tests(suite);
    failed_snapshot_tests(suite);
    return suite.result();
}
#else
int main() {
    std::cout << "SKIP: LocalJobManager not built\n";
    return 77;
}
#endif
