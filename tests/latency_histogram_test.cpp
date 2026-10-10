#include "quill/latency_histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace quill {
namespace {

TEST(LatencyHistogram, EmptyHistogram) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 0U);
    EXPECT_EQ(h.percentile(50), 0U);
    EXPECT_DOUBLE_EQ(h.mean(), 0.0);
}

TEST(LatencyHistogram, RejectsSillyPrecision) {
    EXPECT_THROW(LatencyHistogram(0), std::invalid_argument);
    EXPECT_THROW(LatencyHistogram(17), std::invalid_argument);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
    LatencyHistogram h(5);
    for (std::uint64_t v = 0; v < 32; ++v) {
        EXPECT_EQ(h.bucket_index(v), v);
        EXPECT_EQ(h.bucket_lower_bound(v), v);
        EXPECT_EQ(h.bucket_upper_bound(v), v);
    }
}

TEST(LatencyHistogram, BucketsTileTheRangeWithoutGaps) {
    LatencyHistogram h(4);
    for (std::size_t i = 0; i + 1 < h.bucket_count(); ++i) {
        ASSERT_EQ(h.bucket_upper_bound(i) + 1, h.bucket_lower_bound(i + 1)) << "bucket " << i;
    }
    EXPECT_EQ(h.bucket_upper_bound(h.bucket_count() - 1),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(LatencyHistogram, IndexRoundTripsThroughBounds) {
    LatencyHistogram h(7);
    std::mt19937_64 rng(5);
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t v = rng() >> (rng() % 64);
        const std::size_t idx = h.bucket_index(v);
        ASSERT_LT(idx, h.bucket_count());
        ASSERT_LE(h.bucket_lower_bound(idx), v);
        ASSERT_GE(h.bucket_upper_bound(idx), v);
    }
    EXPECT_EQ(h.bucket_index(std::numeric_limits<std::uint64_t>::max()), h.bucket_count() - 1);
}

TEST(LatencyHistogram, RelativeErrorIsBounded) {
    for (unsigned bits : {3U, 7U, 10U}) {
        LatencyHistogram h(bits);
        const double bound = 1.0 / static_cast<double>(1U << bits);
        for (std::uint64_t v = 1; v < (std::uint64_t{1} << 40); v = v * 3 + 1) {
            const std::size_t idx = h.bucket_index(v);
            const double width =
                static_cast<double>(h.bucket_upper_bound(idx) - h.bucket_lower_bound(idx));
            EXPECT_LE(width / static_cast<double>(v), bound) << "value " << v;
        }
    }
}

TEST(LatencyHistogram, PercentilesTrackExactQuantiles) {
    LatencyHistogram h(7);
    std::vector<std::uint64_t> samples;
    std::mt19937_64 rng(11);
    std::lognormal_distribution<double> dist(6.0, 1.2);
    for (int i = 0; i < 200'000; ++i) {
        const auto v = static_cast<std::uint64_t>(dist(rng));
        samples.push_back(v);
        h.record(v);
    }
    std::sort(samples.begin(), samples.end());
    for (double p : {1.0, 25.0, 50.0, 90.0, 99.0, 99.9}) {
        const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * samples.size())) - 1;
        const double exact = static_cast<double>(samples[rank]);
        const double reported = static_cast<double>(h.percentile(p));
        EXPECT_GE(reported, exact) << "p" << p;
        EXPECT_LE(reported, exact * (1.0 + 1.0 / 128.0) + 1.0) << "p" << p;
    }
    EXPECT_EQ(h.percentile(100), samples.back());
    EXPECT_EQ(h.percentile(0), samples.front());
}

TEST(LatencyHistogram, MeanMinMaxAreExact) {
    LatencyHistogram h;
    h.record(100);
    h.record(200);
    h.record_n(1'000, 2);
    EXPECT_EQ(h.count(), 4U);
    EXPECT_EQ(h.min(), 100U);
    EXPECT_EQ(h.max(), 1'000U);
    EXPECT_DOUBLE_EQ(h.mean(), 575.0);
    EXPECT_NEAR(h.stddev(), 426.47, 0.01);  // population standard deviation
}

TEST(LatencyHistogram, MergeCombinesSamples) {
    LatencyHistogram a;
    LatencyHistogram b;
    a.record(10);
    a.record(20);
    b.record(5);
    b.record(5'000);
    a.merge(b);
    EXPECT_EQ(a.count(), 4U);
    EXPECT_EQ(a.min(), 5U);
    EXPECT_EQ(a.max(), 5'000U);
    EXPECT_DOUBLE_EQ(a.mean(), (10.0 + 20 + 5 + 5000) / 4);

    LatencyHistogram empty;
    empty.merge(a);
    EXPECT_EQ(empty.min(), 5U);

    LatencyHistogram other_precision(3);
    EXPECT_THROW(a.merge(other_precision), std::invalid_argument);
}

TEST(LatencyHistogram, ResetClearsEverything) {
    LatencyHistogram h;
    h.record(42);
    h.reset();
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.percentile(99), 0U);
    h.record(7);
    EXPECT_EQ(h.min(), 7U);
}

TEST(LatencyHistogram, FormatsSummaryAndTable) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    const std::string summary = h.summary();
    EXPECT_NE(summary.find("n=100"), std::string::npos);
    EXPECT_NE(summary.find("p50=50ns"), std::string::npos);
    EXPECT_NE(summary.find("max=100ns"), std::string::npos);
    std::ostringstream table;
    h.print_percentiles(table, "us");
    EXPECT_NE(table.str().find("99.9"), std::string::npos);
    EXPECT_NE(table.str().find("samples 100"), std::string::npos);
}

}  // namespace
}  // namespace quill
