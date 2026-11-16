#include "chipcae/par/gpu_aware.hpp"
namespace chipcae::par {
bool gpu_aware() noexcept {
    return Communicator::world().gpu_aware();
}
} // namespace chipcae::par
