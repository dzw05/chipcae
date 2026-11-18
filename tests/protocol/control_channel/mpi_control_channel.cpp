#include "chipcae/coupling/control_channel.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mpi.h>
#include <stdexcept>

namespace {
bool corrupt_payload = false;
void collective_expect(bool ok, const char* why) {
    int bad = ok ? 0 : 1;
    int any_bad = 0;
    MPI_Allreduce(&bad, &any_bad, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    if (any_bad)
        throw std::runtime_error(why);
}
} // namespace
// Use MPI's profiling interface to inject a rank-local malformed payload.
extern "C" int MPI_Bcast(void* buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm) {
    const int result = PMPI_Bcast(buffer, count, datatype, root, comm);
    int local_rank = 0;
    PMPI_Comm_rank(comm, &local_rank);
    if (result == MPI_SUCCESS && corrupt_payload && local_rank == 1 && datatype == MPI_BYTE &&
        count > 8) {
        static_cast<std::uint8_t*>(buffer)[0] = 255;
        corrupt_payload = false;
    }
    return result;
}
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    try {
        std::string path;
        if (rank == 0) {
            const auto dir =
                std::filesystem::temp_directory_path() /
                ("ctlch-mpi-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directory(dir);
            path = (dir / "control.jsonl").string();
            std::ofstream(path).close();
        }
        int length = static_cast<int>(path.size());
        MPI_Bcast(&length, 1, MPI_INT, 0, MPI_COMM_WORLD);
        path.resize(static_cast<std::size_t>(length));
        MPI_Bcast(path.data(), length, MPI_CHAR, 0, MPI_COMM_WORLD);
        int warnings = 0;
        bool fail_warning = false;
        chipcae::coupling::ControlChannel channel(
            rank == 0 ? path : "/nonexistent/chipcae-ctlch/control.jsonl", [&](const std::string&) {
                ++warnings;
                if (fail_warning)
                    throw std::runtime_error("injected warning failure");
            });
        collective_expect(channel.last_sequence() == 0, "initial MPI sequence");
        const std::array<const char*, 3> actions{"PAUSE", "RESUME", "STOP"};
        for (int round = 0; round < 3; ++round) {
            const auto seq = static_cast<std::uint64_t>(round + 1);
            if (rank == 0) {
                std::ofstream out(path, std::ios::app);
                out << "{\"cmd_seq\":\"" << seq << "\",\"client_request_id\":\"req-" << seq
                    << "\",\"action\":\"" << actions[static_cast<std::size_t>(round)] << "\"}\n";
                // Exercise the root warning sink without affecting accepted commands.
                if (round == 2)
                    out << "{\"cmd_seq\":\"4\",\"action\":\"stop\"}\n";
                if (!out)
                    throw std::runtime_error("append MPI fixture");
            }
            MPI_Barrier(MPI_COMM_WORLD);
            const auto commands = channel.poll({seq, static_cast<double>(round) * 0.1});
            const auto bytes = chipcae::coupling::detail::encode_commands(commands);
            const std::vector<chipcae::io::ControlCommand> expected{
                {seq,
                 "req-" + std::to_string(seq),
                 static_cast<chipcae::io::ControlAction>(round)}};
            const auto expected_bytes = chipcae::coupling::detail::encode_commands(expected);
            std::vector<std::uint8_t> reference = rank == 0 ? bytes : std::vector<std::uint8_t>{};
            int size = static_cast<int>(reference.size());
            MPI_Bcast(&size, 1, MPI_INT, 0, MPI_COMM_WORLD);
            reference.resize(static_cast<std::size_t>(size));
            MPI_Bcast(reference.data(), size, MPI_BYTE, 0, MPI_COMM_WORLD);
            collective_expect(commands.size() == 1 && bytes == expected_bytes &&
                                  bytes == reference && channel.last_sequence() == seq,
                              "MPI command bytes, count and sequence");
        }
        collective_expect(warnings == (rank == 0 ? 1 : 0), "only root may warn");
        bool caught = false;
        try {
            (void)channel.poll({rank == 1 ? 99u : 4u, 0.4});
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()).find("window mismatch") != std::string::npos;
        }
        collective_expect(caught, "all ranks must catch window mismatch");
        caught = false;
        try {
            (void)channel.poll({4u, rank == 1 ? -0.0 : 0.0});
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()).find("window mismatch") != std::string::npos;
        }
        collective_expect(caught, "all ranks must catch time bit mismatch");
        if (rank == 0) {
            std::ofstream out(path, std::ios::app);
            out << "{\"cmd_seq\":\"4\",\"action\":\"PAUSE\"}\n"
                   "{\"cmd_seq\":\"5\",\"action\":\"invalid\"}\n";
        }
        MPI_Barrier(MPI_COMM_WORLD);
        fail_warning = true;
        caught = false;
        try {
            (void)channel.poll({4, 0.4});
        } catch (const std::runtime_error&) {
            caught = true;
        }
        collective_expect(caught && channel.last_sequence() == 4,
                          "read failure must synchronize consumed sequence");
        fail_warning = false;
        if (rank == 0) {
            std::ofstream out(path, std::ios::app);
            out << "{\"cmd_seq\":\"5\",\"action\":\"STOP\"}\n";
        }
        MPI_Barrier(MPI_COMM_WORLD);
        corrupt_payload = true;
        caught = false;
        try {
            (void)channel.poll({4, 0.4});
        } catch (const std::runtime_error&) {
            caught = true;
        }
        corrupt_payload = false;
        collective_expect(caught && channel.last_sequence() == 5,
                          "local decode failure must fail all ranks and preserve sequence");
        collective_expect(channel.poll({3, 0.3}).empty() && channel.last_sequence() == 5,
                          "failed polls must not advance window or replay consumed commands");
        MPI_Barrier(MPI_COMM_WORLD);
        if (rank == 0)
            std::filesystem::remove_all(std::filesystem::path(path).parent_path());
    } catch (const std::exception& e) {
        std::cerr << "rank " << rank << ": " << e.what() << '\n';
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    MPI_Finalize();
}
