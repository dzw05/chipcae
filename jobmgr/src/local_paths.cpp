#include "chipcae/jobmgr/local_paths.hpp"

#include "sha256_stream.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace chipcae::jobmgr {
bool is_uuid(std::string_view id) {
    if (id.size() == 38 && id.front() == '{' && id.back() == '}')
        id = id.substr(1, 36);
    if (id.size() != 36)
        return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return false;
        } else {
            const char c = id[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}
std::string make_run_id() {
    std::random_device random;
    std::uniform_int_distribution<unsigned> distribution(0, 255);
    std::array<unsigned, 16> bytes{};
    for (auto& byte : bytes)
        byte = distribution(random);
    bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
    bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
    constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            id += '-';
        id += hex[bytes[i] >> 4];
        id += hex[bytes[i] & 0x0fU];
    }
    return id;
}
void check_snapshot_ref(std::string_view ref) {
    bool unsafe = ref.starts_with('/') || ref.find('\\') != std::string_view::npos ||
                  ref.find(':') != std::string_view::npos ||
                  ref.find('\0') != std::string_view::npos;
    for (std::size_t begin = 0; begin < ref.size();) {
        const auto end = ref.find('/', begin);
        if (ref.substr(begin, end == std::string_view::npos ? end : end - begin) == "..")
            unsafe = true;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    if (unsafe)
        throw std::runtime_error("Unsafe snapshot reference");
}
namespace {
std::vector<char> temporary_name(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    std::filesystem::create_directories(parent);
    const auto pattern = (parent / (path.filename().string() + ".tmp-XXXXXX")).string();
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    return temporary;
}

void write_complete(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count =
            std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::write(fd, bytes.data(), count);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            throw std::system_error(
                written < 0 ? errno : EIO, std::generic_category(), "Write atomic file");
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
}

class AtomicFile {
public:
    explicit AtomicFile(const std::filesystem::path& path)
        : temporary_(temporary_name(path)), fd_(::mkstemp(temporary_.data())) {
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "Create atomic file");
    }
    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    ~AtomicFile() {
        if (fd_ >= 0)
            ::close(fd_);
        if (!temporary_.empty())
            ::unlink(temporary_.data());
    }
    void write(std::string_view bytes) { write_complete(fd_, bytes); }
    void commit(const std::filesystem::path& path) {
        if (::fsync(fd_) != 0)
            throw std::system_error(errno, std::generic_category(), "Sync atomic file");
        const int closing = fd_;
        fd_ = -1;
        if (::close(closing) != 0)
            throw std::system_error(errno, std::generic_category(), "Close atomic file");
        if (::rename(temporary_.data(), path.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "Rename atomic file");
        temporary_.clear();
    }

private:
    std::vector<char> temporary_;
    int fd_;
};

class SourceFile {
public:
    explicit SourceFile(const std::filesystem::path& path)
        : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
        if (fd_ < 0)
            throw std::runtime_error("Cannot read " + path.string());
    }
    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;
    ~SourceFile() { ::close(fd_); }
    int fd() const { return fd_; }

private:
    int fd_;
};

// Only an initial BOM is accepted. A partial or invalid BOM remains non-JSON.
class JsonPrefix {
public:
    void update(std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            if (state_ == State::done)
                break;
            if (state_ == State::initial) {
                state_ = byte == 0xef ? State::bom_second : State::whitespace;
                if (byte == 0xef)
                    continue;
            } else if (state_ == State::bom_second) {
                state_ = byte == 0xbb ? State::bom_third : State::done;
                continue;
            } else if (state_ == State::bom_third) {
                state_ = byte == 0xbf ? State::whitespace : State::done;
                continue;
            }
            if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
                continue;
            candidate_ = byte == '{' || byte == '[' || byte == '/';
            state_ = State::done;
        }
    }
    bool candidate() const { return candidate_; }

private:
    enum class State { initial, bom_second, bom_third, whitespace, done };
    State state_ = State::initial;
    bool candidate_ = false;
};
} // namespace

void write_file_atomic(const std::filesystem::path& path, std::string_view bytes) {
    AtomicFile output(path);
    output.write(bytes);
    output.commit(path);
}

namespace detail {
StreamedFile stream_copy_hash(const std::filesystem::path& source,
                              const std::filesystem::path& target,
                              const std::string& expected_lowercase) {
    SourceFile input(source);
    std::optional<AtomicFile> output;
    if (!target.empty())
        output.emplace(target);
    auto buffer = std::make_unique_for_overwrite<char[]>(snapshot_chunk_bytes);
    Sha256 hash;
    JsonPrefix prefix;
    StreamedFile result;
    for (;;) {
        const auto count = ::read(input.fd(), buffer.get(), snapshot_chunk_bytes);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw std::runtime_error("Cannot read " + source.string());
        if (count == 0)
            break;
        const std::string_view chunk(buffer.get(), static_cast<std::size_t>(count));
        hash.update(chunk);
        result.bytes += static_cast<std::uintmax_t>(count);
        prefix.update(chunk);
        if (output)
            output->write(chunk);
    }
    result.sha256 = hash.hex_final();
    result.json_candidate = prefix.candidate();
    if (!expected_lowercase.empty() && result.sha256 != expected_lowercase)
        throw std::runtime_error("INPUT_HASH_MISMATCH");
    if (output)
        output->commit(target);
    return result;
}
} // namespace detail
} // namespace chipcae::jobmgr
