#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace quill {

/// Log-linear histogram for latency samples (typically nanoseconds).
///
/// Values below 2^precision_bits are counted exactly. Above that, each power-of-two range
/// is split into 2^precision_bits equal buckets, so every recorded value is reported with
/// a relative error below 2^-precision_bits. This is the same bucketing scheme HdrHistogram
/// uses, without the auto-resizing: the full 64-bit range is always covered.
class LatencyHistogram {
  public:
    explicit LatencyHistogram(unsigned precision_bits = 7);

    void record(std::uint64_t value) noexcept { record_n(value, 1); }
    void record_n(std::uint64_t value, std::uint64_t count) noexcept;

    /// Adds every sample of `other`. Both histograms must use the same precision.
    void merge(const LatencyHistogram& other);
    void reset() noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept;
    [[nodiscard]] double stddev() const noexcept;

    /// Smallest recorded-bucket upper bound such that at least `percentile` percent of
    /// samples are at or below it. Clamped to the exact recorded max.
    [[nodiscard]] std::uint64_t percentile(double percentile) const noexcept;

    [[nodiscard]] unsigned precision_bits() const noexcept { return precision_bits_; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return counts_.size(); }

    /// Bucket mapping, exposed for testing.
    [[nodiscard]] std::size_t bucket_index(std::uint64_t value) const noexcept;
    [[nodiscard]] std::uint64_t bucket_lower_bound(std::size_t index) const noexcept;
    [[nodiscard]] std::uint64_t bucket_upper_bound(std::size_t index) const noexcept;

    /// One-line summary: count, min, percentiles and max.
    [[nodiscard]] std::string summary(const std::string& unit = "ns") const;
    /// Multi-line percentile table in the style of HdrHistogram's output.
    void print_percentiles(std::ostream& out, const std::string& unit = "ns") const;

  private:
    unsigned precision_bits_;
    std::uint64_t sub_buckets_;
    std::vector<std::uint64_t> counts_;
    std::uint64_t count_ = 0;
    std::uint64_t min_ = 0;
    std::uint64_t max_ = 0;
    // Running sums kept in long double so the mean of billions of samples stays exact
    // enough to be useful.
    long double sum_ = 0;
    long double sum_squares_ = 0;
};

}  // namespace quill
