#include "chipcae/core/parallel.hpp"

namespace chipcae {

Communicator Communicator::world() {
#ifdef CHIPCAE_ENABLE_MPI
    // MPI wiring will be added when CHIPCAE_ENABLE_MPI is turned on.
#endif
    return Communicator(0, 1);
}

} // namespace chipcae
