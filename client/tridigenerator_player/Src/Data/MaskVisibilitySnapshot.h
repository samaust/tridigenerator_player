#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct MaskVisibilitySnapshot {
    std::array<std::uint64_t, 4> words{
        ~std::uint64_t{0}, ~std::uint64_t{0},
        ~std::uint64_t{0}, ~std::uint64_t{0}};
    std::uint64_t version = 0;

    bool IsVisible(std::uint8_t id) const {
        return (words[id / 64] & (std::uint64_t{1} << (id % 64))) != 0;
    }
};

class MaskVisibilityPublisher {
public:
    void Publish(const int* values) {
        if (values == nullptr) return;
        std::lock_guard<std::mutex> lock(mutex_);
        words_.fill(0);
        for (std::size_t id = 0; id < 256; ++id) {
            if (values[id] != 0) {
                words_[id / 64] |= std::uint64_t{1} << (id % 64);
            }
        }
        ++version_;
    }

    MaskVisibilitySnapshot Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {words_, version_};
    }

private:
    mutable std::mutex mutex_;
    std::array<std::uint64_t, 4> words_{
        ~std::uint64_t{0}, ~std::uint64_t{0},
        ~std::uint64_t{0}, ~std::uint64_t{0}};
    std::uint64_t version_ = 0;
};
