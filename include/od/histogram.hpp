#pragma once
// Fixed-memory latency histogram with log-linear buckets (the HdrHistogram idea,
// simplified): each power-of-two range is split into 16 linear sub-buckets, so
// any recorded value is reported within ~6% relative error, from 1 ns to ~1 hour,
// in 8 KiB of counters. Recording is a few integer ops and never allocates, so
// it is safe on a hot path. Not thread-safe: one histogram per thread.

#include <array>
#include <bit>
#include <cstdint>

namespace od {

class LatencyHistogram {
public:
    static constexpr int kSubBits = 4;
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kBuckets = 64 * kSub;

    void record(std::uint64_t v) {
        ++counts_[index(v)];
        ++count_;
        if (v > max_) max_ = v;
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t max() const { return max_; }

    // Upper edge of the bucket holding the q-th quantile (q in [0, 1]).
    std::uint64_t percentile(double q) const {
        if (count_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(q * static_cast<double>(count_ - 1)) + 1;
        std::uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= target) return upper_edge(i) < max_ ? upper_edge(i) : max_;
        }
        return max_;
    }

    void merge(const LatencyHistogram& o) {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        if (o.max_ > max_) max_ = o.max_;
    }

    void reset() { *this = LatencyHistogram{}; }

private:
    // Values < 16 map 1:1. Otherwise: exponent e = bit width - 1, and the next 4 bits
    // below the leading one pick the sub-bucket.
    static int index(std::uint64_t v) {
        if (v < kSub) return static_cast<int>(v);
        const int e = std::bit_width(v) - 1;
        const int sub = static_cast<int>((v >> (e - kSubBits)) & (kSub - 1));
        return (e - kSubBits + 1) * kSub + sub;
    }

    static std::uint64_t upper_edge(int i) {
        if (i < kSub) return static_cast<std::uint64_t>(i);
        const int e = i / kSub + kSubBits - 1;
        const std::uint64_t sub = static_cast<std::uint64_t>(i % kSub);
        const std::uint64_t base = (std::uint64_t{1} << e) | (sub << (e - kSubBits));
        return base + (std::uint64_t{1} << (e - kSubBits)) - 1;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t max_ = 0;
};

}  // namespace od
