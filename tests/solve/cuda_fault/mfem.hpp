#pragma once

#include <ostream>

// A test-only external dependency stand-in for compiling the real main.cpp.
#ifdef TEST_MFEM_CUDA
#define MFEM_USE_CUDA
#endif

namespace mfem {
class Device {
public:
    explicit Device(const char*) {}
    void Print(std::ostream&) const {}
};
} // namespace mfem
