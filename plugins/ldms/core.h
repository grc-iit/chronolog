#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chronolog::ldms
{
// Vyukov bounded queue restricted to lock-free push from many producers and pop from one consumer.
template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(size_t capacity)
    {
        size_t size = 2;
        while(size < capacity) size <<= 1;
        mask_ = size - 1;
        cells_ = std::make_unique<Cell[]>(size);
        for(size_t i = 0; i < size; ++i) cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
    bool push(T value)
    {
        size_t pos = head_.load(std::memory_order_relaxed);
        for(;;)
        {
            Cell& cell = cells_[pos & mask_];
            const auto diff =
                    static_cast<intptr_t>(cell.sequence.load(std::memory_order_acquire)) - static_cast<intptr_t>(pos);
            if(diff == 0)
            {
                if(head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.value = std::move(value);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if(diff < 0)
                return false;
            else
                pos = head_.load(std::memory_order_relaxed);
        }
    }
    bool pop(T& value)
    {
        Cell& cell = cells_[tail_ & mask_];
        if(cell.sequence.load(std::memory_order_acquire) != tail_ + 1)
            return false;
        value = std::move(cell.value);
        cell.sequence.store(tail_ + mask_ + 1, std::memory_order_release);
        ++tail_;
        return true;
    }
    size_t slots() const { return mask_ + 1; }

private:
    struct Cell
    {
        std::atomic<size_t> sequence{0};
        T value{};
    };
    std::unique_ptr<Cell[]> cells_;
    size_t mask_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) size_t tail_{0};
};

// Stories and chronicles become archive file names, so anything outside [A-Za-z0-9_-] is folded to '_'.
inline std::string sanitizeName(const char* raw)
{
    std::string out;
    for(const char* p = raw ? raw : ""; *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool keep =
                (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
        out.push_back(keep ? static_cast<char>(c) : '_');
    }
    return out;
}

// The Keeper accepts an Unsynced physical reading only inside [L - A, L + S] (I8.9). The margins sit inside the
// default 15 s and 60 s so queueing and retry delay cannot push an accepted-looking sample out of range.
constexpr int64_t kWindowBackNs = 10'000'000'000;
constexpr int64_t kWindowAheadNs = 30'000'000'000;
inline bool inKeeperWindow(int64_t sample_ns, int64_t now_ns)
{
    return sample_ns >= now_ns - kWindowBackNs && sample_ns <= now_ns + kWindowAheadNs;
}
} // namespace chronolog::ldms
