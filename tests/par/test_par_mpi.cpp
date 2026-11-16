#include "chipcae/core/communicator.hpp"

#include <array>
#include <iostream>
#include <mpi.h>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rc = 0;
    try {
        const auto comm = chipcae::Communicator::world();
        int mpi_rank = 0, mpi_size = 0;
        MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
        MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
        require(comm.rank() == mpi_rank, "rank");
        require(comm.size() == mpi_size, "size");
        require(mpi_size >= 2, "need at least two ranks");

        std::array<double, 3> values{
            static_cast<double>(comm.rank() + 1), 1.0, static_cast<double>(comm.rank())};
        comm.allreduce_sum(values);
        const double n = static_cast<double>(comm.size());
        require(values[0] == n * (n + 1) / 2.0, "allreduce_sum rank terms");
        require(values[1] == n, "allreduce_sum ones");
        require(values[2] == n * (n - 1) / 2.0, "allreduce_sum ranks");

        values = {static_cast<double>(comm.rank() + 1), 1.0, static_cast<double>(comm.rank())};
        comm.deterministic_allreduce_sum(values);
        require(values[0] == n * (n + 1) / 2.0, "deterministic_allreduce_sum rank terms");
        require(values[1] == n, "deterministic_allreduce_sum ones");
        require(values[2] == n * (n - 1) / 2.0, "deterministic_allreduce_sum ranks");

        if (comm.rank() == 0)
            std::cout << "par MPI communication passed; ranks=" << comm.size() << '\n';
    } catch (const std::exception& ex) {
        std::cerr << "par MPI communication failed: " << ex.what() << '\n';
        rc = 1;
    }
    MPI_Finalize();
    return rc;
}
