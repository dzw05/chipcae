#include "chipcae/coupling/control_channel.hpp"

#include <array>
#include <bit>
#include <climits>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif

namespace chipcae::coupling {
namespace {
bool active() noexcept {
#ifdef CHIPCAE_ENABLE_MPI
    int initialized = 0, finalized = 0;
    MPI_Initialized(&initialized);
    if (initialized)
        MPI_Finalized(&finalized);
    return initialized && !finalized;
#else
    return false;
#endif
}
int rank() {
    int result = 0;
#ifdef CHIPCAE_ENABLE_MPI
    if (active())
        MPI_Comm_rank(MPI_COMM_WORLD, &result);
#endif
    return result;
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
class Decoder {
public:
    explicit Decoder(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint64_t get(unsigned width) {
        require(width);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= std::uint64_t(bytes_[offset_++]) << (8 * i);
        return value;
    }
    std::string string() {
        auto size = get(4);
        require(size);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return value;
    }
    bool boolean() {
        auto value = get(1);
        if (value > 1)
            throw std::runtime_error("control channel invalid boolean");
        return value != 0;
    }
    std::size_t remaining() const { return bytes_.size() - offset_; }

private:
    void require(std::uint64_t size) const {
        if (size > remaining())
            throw std::runtime_error("control channel truncated payload");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};
#ifdef CHIPCAE_ENABLE_MPI
void check(int result) {
    if (result != MPI_SUCCESS)
        throw std::runtime_error("control channel collective failed");
}
#endif
} // namespace

namespace detail {
std::vector<std::uint8_t> encode_commands(const std::vector<io::ControlCommand>& commands) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    std::vector<std::uint8_t> bytes;
    put(bytes, commands.size(), 8);
    for (const auto& command : commands) {
        if (command.client_request_id.size() > UINT32_MAX)
            throw std::runtime_error("control channel request ID too large");
        auto action = static_cast<unsigned>(command.action);
        if (action > 5)
            throw std::runtime_error("control channel invalid action");
        put(bytes, command.cmd_seq, 8);
        put(bytes, command.client_request_id.size(), 4);
        bytes.insert(
            bytes.end(), command.client_request_id.begin(), command.client_request_id.end());
        put(bytes, action, 1);
        put(bytes, command.has_interval_steps, 1);
        put(bytes, command.output_interval_steps, 8);
        put(bytes, command.has_interval_time, 1);
        put(bytes, std::bit_cast<std::uint64_t>(command.output_interval_time), 8);
    }
    return bytes;
}
std::vector<io::ControlCommand> decode_commands(std::span<const std::uint8_t> bytes) {
    Decoder decoder(bytes);
    auto count = decoder.get(8);
    if (count > decoder.remaining() / 31)
        throw std::runtime_error("control channel invalid count");
    std::vector<io::ControlCommand> commands;
    commands.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        io::ControlCommand command;
        command.cmd_seq = decoder.get(8);
        command.client_request_id = decoder.string();
        auto action = decoder.get(1);
        if (action > 5)
            throw std::runtime_error("control channel invalid action");
        command.action = static_cast<io::ControlAction>(action);
        command.has_interval_steps = decoder.boolean();
        command.output_interval_steps = decoder.get(8);
        command.has_interval_time = decoder.boolean();
        command.output_interval_time = std::bit_cast<double>(decoder.get(8));
        commands.push_back(std::move(command));
    }
    if (decoder.remaining())
        throw std::runtime_error("control channel trailing data");
    return commands;
}
} // namespace detail

ControlChannel::ControlChannel(std::filesystem::path path,
                               std::function<void(const std::string&)> warning_sink) {
    if (rank() == 0)
        reader_ = std::make_unique<io::ControlFileReader>(std::move(path), std::move(warning_sink));
}
std::vector<io::ControlCommand> ControlChannel::poll(const WindowMark& completed) {
    const bool distributed = active();
    int rollback = completed.window < previous_window_;
#ifdef CHIPCAE_ENABLE_MPI
    if (distributed) {
        std::array<std::uint64_t, 2> mark{completed.window,
                                          std::bit_cast<std::uint64_t>(completed.time)};
        std::array<std::uint64_t, 2> minimum{}, maximum{};
        check(MPI_Allreduce(mark.data(), minimum.data(), 2, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD));
        check(MPI_Allreduce(mark.data(), maximum.data(), 2, MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD));
        if (minimum != maximum)
            throw std::runtime_error("control channel window mismatch");
        check(MPI_Allreduce(MPI_IN_PLACE, &rollback, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD));
    }
#endif
    if (rollback)
        throw std::invalid_argument("control channel window rollback");
    std::vector<std::uint8_t> bytes;
    std::exception_ptr failure;
    int status = 0;
    if (rank() == 0) {
        try {
            bytes = detail::encode_commands(reader_->read_available());
            if (bytes.size() > INT_MAX)
                throw std::runtime_error("control channel payload too large");
        } catch (...) {
            failure = std::current_exception();
            status = 1;
        }
        last_sequence_ = reader_->last_sequence();
    }
#ifdef CHIPCAE_ENABLE_MPI
    if (distributed) {
        check(MPI_Bcast(&last_sequence_, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD));
        check(MPI_Bcast(&status, 1, MPI_INT, 0, MPI_COMM_WORLD));
    }
#endif
    if (status) {
        if (failure)
            std::rethrow_exception(failure);
        throw std::runtime_error("control channel root read/encode failed");
    }
#ifdef CHIPCAE_ENABLE_MPI
    if (distributed) {
        std::array<std::uint8_t, 8> length{};
        const auto local_size = static_cast<std::uint64_t>(bytes.size());
        for (unsigned i = 0; i < length.size(); ++i)
            length[i] = static_cast<std::uint8_t>(local_size >> (8 * i));
        check(MPI_Bcast(length.data(), 8, MPI_BYTE, 0, MPI_COMM_WORLD));
        const auto size = Decoder(length).get(8);
        int oversized = size > INT_MAX;
        check(MPI_Allreduce(MPI_IN_PLACE, &oversized, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD));
        if (oversized)
            throw std::runtime_error("control channel payload too large");
        // Coordinate allocation failures before entering the payload collective.
        int allocation_failed = 0;
        try {
            bytes.resize(size);
        } catch (...) {
            allocation_failed = 1;
        }
        check(MPI_Allreduce(MPI_IN_PLACE, &allocation_failed, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD));
        if (allocation_failed)
            throw std::runtime_error("control channel payload allocation failed");
        check(MPI_Bcast(bytes.data(), static_cast<int>(size), MPI_BYTE, 0, MPI_COMM_WORLD));
    }
#else
    (void)distributed;
#endif
    std::vector<io::ControlCommand> commands;
    int decode_failed = 0;
    try {
        commands = detail::decode_commands(bytes);
    } catch (...) {
        failure = std::current_exception();
        decode_failed = 1;
    }
#ifdef CHIPCAE_ENABLE_MPI
    if (distributed)
        check(MPI_Allreduce(MPI_IN_PLACE, &decode_failed, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD));
#endif
    if (decode_failed) {
        if (failure)
            std::rethrow_exception(failure);
        throw std::runtime_error("control channel decode failed on another rank");
    }
    previous_window_ = completed.window;
    return commands;
}
std::uint64_t ControlChannel::last_sequence() const noexcept {
    return reader_ ? reader_->last_sequence() : last_sequence_;
}
io::ControlAck make_control_ack(const io::ControlCommand& command,
                                std::uint32_t status,
                                const WindowMark& completed,
                                std::string message) {
    return {command.cmd_seq,
            command.client_request_id,
            status,
            completed.window,
            std::move(message),
            completed.time};
}
} // namespace chipcae::coupling
