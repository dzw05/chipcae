#include "mfd_local.hpp"

namespace chipcae::solvers::fvm::detail {
Expected<LocalMfd>
build_local_mfd(const FvGrid&, std::size_t, const std::array<double, 3>&, const std::string&) {
    return mesh::Error{"fvm/mfd", "NOT_IMPLEMENTED: local MFD operator (test-first stub)"};
}
} // namespace chipcae::solvers::fvm::detail
