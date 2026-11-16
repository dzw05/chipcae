// Compile-only CUDA smoke test. Nightly nvcc -c, never launched, not linked into chipcae.

extern "C" __global__ void chipcae_cuda_smoke(int* values, int count) {
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index < count) {
        values[index] = index;
    }
}
