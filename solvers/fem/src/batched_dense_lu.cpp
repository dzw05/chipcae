#include "batched_dense_lu.hpp"

#include <stdexcept>

namespace chipcae::fem::detail {

void BatchedDenseLU::resize(std::vector<std::int64_t>, std::vector<int>) {
    throw std::logic_error("not implemented");
}

std::size_t BatchedDenseLU::blocks() const {
    throw std::logic_error("not implemented");
}

int BatchedDenseLU::size(std::size_t) const {
    throw std::logic_error("not implemented");
}

std::int64_t BatchedDenseLU::first(std::size_t) const {
    throw std::logic_error("not implemented");
}

double* BatchedDenseLU::block(std::size_t) {
    throw std::logic_error("not implemented");
}

std::size_t BatchedDenseLU::dense_size() const {
    throw std::logic_error("not implemented");
}

std::int64_t BatchedDenseLU::factor() {
    throw std::logic_error("not implemented");
}

bool BatchedDenseLU::solve(double*) const {
    throw std::logic_error("not implemented");
}

} // namespace chipcae::fem::detail
