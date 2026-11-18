#include "chipcae/core/crash_diag.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace chipcae::core {
namespace {
constexpr std::array fatal_signals{SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
constexpr std::size_t line_capacity = 64;
struct Snapshot {
    std::uint64_t window{};
    double time{};
    char lines[5][line_capacity]{};
    std::size_t lengths[5]{};
};
// The first handler announces acquisition through handling before loading the
// publication. The single ordinary-code writer checks that flag before loading
// its current slot and never starts another update after acquisition begins.
// One update already in flight can finish only in the other slot. Sequentially
// consistent flag/publication operations let the handler read either complete
// snapshot without waiting for an interrupted writer or racing slot reuse.
static_assert(std::atomic<int>::is_always_lock_free);
std::array<Snapshot, 2> slots;
std::atomic<int> published{0};
std::atomic_flag handling = ATOMIC_FLAG_INIT;
int pinned_slot = 0;
int signal_fd = -1;
std::atomic<bool> active{false};
static_assert(std::atomic<bool>::is_always_lock_free);
std::array<struct sigaction, 5> previous_actions{};
std::size_t saved_actions = 0;
std::filesystem::path signal_path;
std::filesystem::path json_path;
int installed_rank = 0;

[[noreturn]] void system_failure(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void format_snapshot(Snapshot& snapshot, std::uint64_t window, double time) {
    snapshot.window = window;
    snapshot.time = time;
    char digits[20];
    std::size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + window % 10);
        window /= 10;
    } while (window != 0);
    for (std::size_t i = 0; i < fatal_signals.size(); ++i) {
        auto& line = snapshot.lines[i];
        std::size_t pos = 0;
        for (const char c : std::string_view("window="))
            line[pos++] = c;
        for (auto j = count; j != 0; --j)
            line[pos++] = digits[j - 1];
        for (const char c : std::string_view(" signal="))
            line[pos++] = c;
        unsigned int sig = static_cast<unsigned int>(fatal_signals[i]);
        char signal_digits[std::numeric_limits<unsigned int>::digits10 + 1];
        std::size_t signal_count = 0;
        do {
            signal_digits[signal_count++] = static_cast<char>('0' + sig % 10);
            sig /= 10;
        } while (sig != 0);
        while (signal_count != 0)
            line[pos++] = signal_digits[--signal_count];
        line[pos++] = '\n';
        snapshot.lengths[i] = pos;
    }
}

// BEGIN SIGNAL WRITER: only preformatted bytes and bounded write attempts.
static void write_signal_line(int sig) {
    unsigned int index = 0;
    while (index < 5 && fatal_signals[index] != sig)
        ++index;
    if (index == 5 || signal_fd < 0)
        return;
    const auto& snapshot = slots[pinned_slot];
    const char* bytes = snapshot.lines[index];
    std::size_t offset = 0;
    unsigned int attempts = 0;
    while (offset < snapshot.lengths[index] && attempts < 64) {
        ++attempts;
        const auto written = ::write(signal_fd, bytes + offset, snapshot.lengths[index] - offset);
        if (written > 0)
            offset += written;
        else if (written == 0)
            break;
    }
}
// END SIGNAL WRITER

void remove_empty_signal() noexcept {
    struct stat info{};
    if (signal_fd >= 0 && ::fstat(signal_fd, &info) == 0 && info.st_size == 0) {
        std::error_code ignored;
        std::filesystem::remove(signal_path, ignored);
    }
}

std::string json_quote(std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : input) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    result += '"';
    return result;
}

template <typename Number>
std::string number(Number value) {
    char buffer[128];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{})
        throw std::runtime_error("cannot format diagnostic number");
    return {buffer, result.ptr};
}

bool valid_category(std::string_view category) {
    constexpr std::array<std::string_view, 11> categories{"FAILURE_CATEGORY_UNSPECIFIED",
                                                          "FAILURE_CATEGORY_CASE_INVALID",
                                                          "FAILURE_CATEGORY_DIVERGENCE",
                                                          "FAILURE_CATEGORY_IO",
                                                          "FAILURE_CATEGORY_MPI",
                                                          "FAILURE_CATEGORY_SCHEDULER",
                                                          "FAILURE_CATEGORY_MEMORY",
                                                          "FAILURE_CATEGORY_GPU",
                                                          "FAILURE_CATEGORY_LAUNCH",
                                                          "FAILURE_CATEGORY_INTERNAL",
                                                          "FAILURE_CATEGORY_JOB_MANAGER"};
    for (const auto allowed : categories)
        if (category == allowed)
            return true;
    return false;
}
} // namespace

void CrashDiag::install(const std::filesystem::path& run_dir, int rank) {
    if (rank < 0)
        throw std::invalid_argument("diagnostic rank must be nonnegative");
    finalize();
    const auto directory = run_dir / "diag";
    std::filesystem::create_directories(directory);
    signal_path = directory / ("rank-" + number(rank) + ".signal");
    json_path = directory / ("rank-" + number(rank) + ".json");
    bool created = false;
    try {
        signal_fd =
            ::open(signal_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_APPEND, 0600);
        created = signal_fd >= 0;
        if (signal_fd < 0 && errno == EEXIST)
            signal_fd = ::open(signal_path.c_str(), O_WRONLY | O_CLOEXEC | O_APPEND | O_NONBLOCK);
        if (signal_fd < 0)
            system_failure("open diagnostic signal file");
        struct stat info{};
        if (::fstat(signal_fd, &info) != 0)
            system_failure("stat diagnostic signal file");
        if (!S_ISREG(info.st_mode) || info.st_size != 0)
            throw std::runtime_error("diagnostic signal file must be an empty regular file");
        for (auto& slot : slots) {
            format_snapshot(slot, 0, 0.0);
        }
        published.store(0);
        handling.clear();
        installed_rank = rank;
        struct sigaction action{};
        action.sa_handler = CrashDiag::on_signal;
        ::sigemptyset(&action.sa_mask);
        for (const int sig : fatal_signals)
            ::sigaddset(&action.sa_mask, sig);
        for (std::size_t i = 0; i < fatal_signals.size(); ++i) {
            if (::sigaction(fatal_signals[i], &action, &previous_actions[i]) != 0)
                system_failure("install diagnostic signal handler");
            ++saved_actions;
        }
        active.store(true);
    } catch (...) {
        while (saved_actions != 0) {
            const auto i = --saved_actions;
            ::sigaction(fatal_signals[i], &previous_actions[i], nullptr);
        }
        if (created)
            remove_empty_signal();
        if (signal_fd >= 0)
            ::close(signal_fd);
        signal_fd = -1;
        signal_path.clear();
        json_path.clear();
        throw;
    }
}

void CrashDiag::set_window(std::uint64_t window, double time) {
    if (!installed())
        return;
    if (!std::isfinite(time))
        throw std::invalid_argument("diagnostic time must be finite");
    if (handling.test())
        return;
    const int next = 1 - published.load();
    format_snapshot(slots[next], window, time);
    published.store(next);
}

void CrashDiag::on_error(const CrashError& error) {
    if (!installed())
        throw std::logic_error("crash diagnostics are not installed");
    if (!valid_category(error.category))
        throw std::invalid_argument("invalid diagnostic failure category");
    const auto& snapshot = slots[published.load()];
    const std::string json =
        "{\"schema\":\"chipcae.diag.rank/0.1\",\"rank\":" + number(installed_rank) +
        ",\"category\":" + json_quote(error.category) + ",\"code\":" + json_quote(error.code) +
        ",\"error_code\":" + number(error.error_code) + ",\"module\":" + json_quote(error.module) +
        ",\"window\":" + number(snapshot.window) + ",\"time\":" + number(snapshot.time) +
        ",\"message\":" + json_quote(error.message) + "}\n";
    auto temporary = json_path.string() + ".tmp-XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    int fd = ::mkstemp(name.data());
    if (fd < 0)
        system_failure("create diagnostic JSON temporary file");
    try {
        std::size_t offset = 0;
        while (offset < json.size()) {
            const auto written = ::write(fd, json.data() + offset, json.size() - offset);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                system_failure("write diagnostic JSON");
            }
            if (written == 0)
                throw std::runtime_error("zero-length diagnostic JSON write");
            offset += static_cast<std::size_t>(written);
        }
        const int closing = fd;
        fd = -1; // Do not retry close after EINTR: it may already have released the fd.
        if (::close(closing) != 0)
            system_failure("close diagnostic JSON");
        std::filesystem::rename(name.data(), json_path);
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        ::unlink(name.data());
        throw;
    }
}

// BEGIN SIGNAL HANDLER: all atomics below are always lock-free.
void CrashDiag::on_signal(int sig) {
    if (!handling.test_and_set()) {
        pinned_slot = published.load();
        write_signal_line(sig);
    }
    ::_exit(128 + sig);
}
// END SIGNAL HANDLER

void CrashDiag::finalize() {
    if (!active.exchange(false))
        return;
    while (saved_actions != 0) {
        const auto i = --saved_actions;
        ::sigaction(fatal_signals[i], &previous_actions[i], nullptr);
    }
    remove_empty_signal();
    ::close(signal_fd);
    signal_fd = -1;
    signal_path.clear();
    json_path.clear();
    installed_rank = 0;
}

bool CrashDiag::installed() noexcept {
    return active.load();
}
} // namespace chipcae::core
