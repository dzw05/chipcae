#include <mfem.hpp>
#include <HYPRE.h>
#include <hdf5.h>
int main() {
  const hid_t acc = H5F_ACC_RDONLY;
  (void)acc;
  (void)sizeof(HYPRE_Int);
  mfem::Mesh *mesh = nullptr;
  (void)mesh;
  return 0;
}
