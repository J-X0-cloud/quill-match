#include "quill/latency_histogram.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace quill {

LatencyHistogram::LatencyHistogram(unsigned precision_bits)
    : precision_bits_(precision_bits), sub_buckets_(std::uint64_t{1} << precision_bits) {
    if (precision_bits < 1 || precision_bits > 16) {
        throw std::invalid_argument("histogram precision must be between 1 and 16 bits");
    }
    // Octave 0 is the exact region [0, 2^p); octaves 1..(64 - p) cover the rest of the
    // 64-bit range with 2^p buckets each.
    const std::size_t octaves = 64 - precision_bits_ + 1;
    counts_.assign(octaves * sub_buckets_, 0);
}

std::size_t LatencyHistogram::bucket_index(std::uint64_t value) const noexcept {
    if (value < sub_buckets_) {
        return static_cast<std::size_t>(value);
    }
    const auto msb = static_cast<unsigned>(std::bit_width(value) - 1);
    const unsigned shift = msb - precision_bits_;
    const std::uint64_t mantissa = value >> shift;  // in [2^p, 2^(p+1))
    const std::uint64_t octave = shift + 1;
    return static_cast<std::size_t>(octave * sub_buckets_ + (mantissa - sub_buckets_));
}

std::uint64_t LatencyHistogram::bucket_lower_bound(std::size_t index) const noexcept {
    const std::uint64_t octave = index >> precision_bits_;
    const std::uint64_t sub = index & (sub_buckets_ - 1);
    if (octave == 0) {
        return sub;
    }
    return (sub_buckets_ + sub) << (octave - 1);
}

std::uint64_t LatencyHistogram::bucket_upper_bound(std::size_t index) const noexcept {
    const std::uint64_t octave = index >> precision_bits_;
    if (octave == 0) {
        return bucket_lower_bound(index);
    }
    const std::uint64_t width = std::uint64_t{1} << (octave - 1);
    return bucket_lower_bound(index) + (width - 1);
}

void LatencyHistogram::record_n(std::uint64_t value, std::uint64_t count) noexcept {
    if (count == 0) {
        return;
    }
    counts_[bucket_index(value)] += count;
    if (count_ == 0 || value < min_) {
        min_ = value;
    }
    max_ = std::max(max_, value);
    count_ += count;
    const auto v = static_cast<long double>(value);
    const auto n = static_cast<long double>(count);
    sum_ += v * n;
    sum_squares_ += v * v * n;
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
    if (other.precision_bits_ != precision_bits_) {
        throw std::invalid_argument("cannot merge histograms of different precision");
    }
    if (other.count_ == 0) {
        return;
    }
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        counts_[i] += other.counts_[i];
    }
    min_ = count_ == 0 ? other.min_ : std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    count_ += other.count_;
    sum_ += other.sum_;
    sum_squares_ += other.sum_squares_;
}

void LatencyHistogram::reset() noexcept {
    std::fill(counts_.begin(), counts_.end(), 0);
    count_ = 0;
    min_ = 0;
    max_ = 0;
    sum_ = 0;
    sum_squares_ = 0;
}

double LatencyHistogram::mean() const noexcept {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_ / static_cast<long double>(count_));
}

double LatencyHistogram::stddev() const noexcept {
    if (count_ < 2) {
        return 0.0;
    }
    const long double n = static_cast<long double>(count_);
    const long double m = sum_ / n;
    const long double variance = std::max<long double>(0, sum_squares_ / n - m * m);
    return static_cast<double>(std::sqrt(variance));
}

std::uint64_t LatencyHistogram::percentile(double percentile) const noexcept {
    if (count_ == 0) {
        return 0;
    }
    percentile = std::clamp(percentile, 0.0, 100.0);
    auto target =
        static_cast<std::uint64_t>(std::ceil(percentile / 100.0 * static_cast<double>(count_)));
    target = std::clamp<std::uint64_t>(target, 1, count_);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        seen += counts_[i];
        if (seen >= target) {
            return std::clamp(bucket_upper_bound(i), min_, max_);
        }
    }
    return max_;
}

std::string LatencyHistogram::summary(const std::string& unit) const {
    std::ostringstream out;
    out << "n=" << count_ << " min=" << min() << unit << " p50=" << percentile(50) << unit
        << " p90=" << percentile(90) << unit << " p99=" << percentile(99) << unit
        << " p99.9=" << percentile(99.9) << unit << " max=" << max_ << unit;
    return out.str();
}

void LatencyHistogram::print_percentiles(std::ostream& out, const std::string& unit) const {
    static constexpr double kPoints[] = {0, 50, 75, 90, 95, 99, 99.9, 99.99, 100};
    out << std::left << std::setw(12) << "percentile" << std::right << std::setw(14)
        << ("value (" + unit + ")") << '\n';
    for (const double p : kPoints) {
        std::ostringstream label;
        label << p;
        out << std::left << std::setw(12) << label.str() << std::right << std::setw(14)
            << percentile(p) << '\n';
    }
    out << std::fixed << std::setprecision(1) << "mean " << mean() << unit << ", stddev "
        << stddev() << unit << ", samples " << count_ << '\n';
    out.unsetf(std::ios::fixed);
}

}  // namespace quill
