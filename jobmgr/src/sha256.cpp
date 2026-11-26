#include "chipcae/jobmgr/local_paths.hpp"
#include "sha256_stream.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

namespace chipcae::jobmgr {
namespace {
void compress(std::array<std::uint32_t, 8>& state, const unsigned char* block) noexcept {
    constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            w[i] = (w[i] << 8) | block[4 * i + j];
    for (std::size_t i = 16; i < 64; ++i)
        w[i] = w[i - 16] + (std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
               w[i - 7] + (std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const auto t = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                       ((e & f) ^ (~e & g)) + k[i] + w[i];
        const auto u =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t;
        d = c;
        c = b;
        b = a;
        a = t + u;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
} // namespace

namespace detail {
Sha256::Sha256() noexcept
    : state_{0x6a09e667,
             0xbb67ae85,
             0x3c6ef372,
             0xa54ff53a,
             0x510e527f,
             0x9b05688c,
             0x1f83d9ab,
             0x5be0cd19} {}

void Sha256::update(const void* data, std::size_t size) noexcept {
    if (size == 0)
        return;
    const auto* bytes = static_cast<const unsigned char*>(data);
    total_bytes_ += static_cast<std::uint64_t>(size);
    if (block_used_ != 0) {
        const auto count = std::min(size, block_.size() - block_used_);
        std::memcpy(block_.data() + block_used_, bytes, count);
        block_used_ += count;
        bytes += count;
        size -= count;
        if (block_used_ == block_.size()) {
            compress(state_, block_.data());
            block_used_ = 0;
        }
    }
    while (size >= block_.size()) {
        compress(state_, bytes);
        bytes += block_.size();
        size -= block_.size();
    }
    if (size != 0) {
        std::memcpy(block_.data(), bytes, size);
        block_used_ = size;
    }
}

std::string Sha256::hex_final() {
    const auto bits = total_bytes_ * 8;
    block_[block_used_++] = 0x80;
    if (block_used_ > 56) {
        std::fill(block_.begin() + block_used_, block_.end(), 0);
        compress(state_, block_.data());
        block_used_ = 0;
    }
    std::fill(block_.begin() + block_used_, block_.begin() + 56, 0);
    for (std::size_t i = 0; i < 8; ++i)
        block_[63 - i] = static_cast<unsigned char>(bits >> (8 * i));
    compress(state_, block_.data());
    constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < state_.size(); ++i)
        for (std::size_t j = 0; j < 8; ++j)
            result[8 * i + j] = hex[(state_[i] >> (28 - 4 * j)) & 0xfU];
    return result;
}
} // namespace detail

std::string sha256_hex(std::string_view bytes) {
    detail::Sha256 h;
    h.update(bytes);
    return h.hex_final();
}
} // namespace chipcae::jobmgr
