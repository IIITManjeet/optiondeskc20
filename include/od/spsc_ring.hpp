#pragma once
// Bounded single-producer / single-consumer ring buffer, lock-free.
//
// The feed thread pushes, the pricer thread pops. With exactly one writer per
// index, no CAS is needed: the producer owns `head_`, the consumer owns `tail_`,
// and each only *reads* the other's index.
//
// Memory ordering:
//   producer: write slot, then head_.store(release)  -> slot is visible before the index
//   consumer: head_.load(acquire), then read slot     -> sees the slot the index promised
// and symmetrically for tail_ so the producer never overwrites a slot still being read.
//
// Performance details:
//   - head_ and tail_ live on separate cache lines; otherwise every push invalidates
//     the consumer's line and vice versa (false sharing).
//   - Each side keeps a cached copy of the other's index and only re-reads the shared
//     atomic when the cache says the ring looks full/empty. Most operations then
//     touch no shared cache line at all.
//   - Capacity is a power of two so wrap-around is a mask, not a modulo.

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace od {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "ring slots are copied, keep T trivially copyable");

public:
    explicit SpscRing(std::size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), slots_(std::make_unique<T[]>(capacity_pow2)) {
        if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0) throw std::invalid_argument("capacity must be a power of two");
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer only. Returns false if full (caller decides: drop, spin, or count).
    bool try_push(const T& v) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_cache_ > mask_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head - tail_cache_ > mask_) return false;
        }
        slots_[head & mask_] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    bool try_pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) return false;
        }
        out = slots_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate when called concurrently; exact from either side when the other is idle.
    std::size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    std::size_t capacity() const { return mask_ + 1; }

private:
    const std::size_t mask_;
    std::unique_ptr<T[]> slots_;

    // Producer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;
    // Consumer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;
    // Keep whatever follows this object off the consumer's line.
    char pad_[kCacheLine - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
};

}  // namespace od
