// chipcae/material/state_io.hpp —— 主机侧内存字节流（D12 的 save/restore 载体）
#pragma once

#include "chipcae/material/state_store.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace chipcae::material {

class BlobStateWriter final : public StateWriter {
public:
    void write(const void* data, std::size_t bytes) override {
        const auto* p = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), p, p + bytes);
    }
    const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

class BlobStateReader final : public StateReader {
public:
    explicit BlobStateReader(const std::vector<std::byte>& bytes) : bytes_(bytes) {}
    void read(void* data, std::size_t bytes) override {
        if (offset_ + bytes > bytes_.size())
            throw std::runtime_error(
                "chipcae::material::BlobStateReader: unexpected end of stream");
        std::memcpy(data, bytes_.data() + offset_, bytes);
        offset_ += bytes;
    }

private:
    const std::vector<std::byte>& bytes_;
    std::size_t offset_ = 0;
};

} // namespace chipcae::material
