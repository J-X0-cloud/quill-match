#pragma once

#include <cstdint>
#include <string>

#include "quill/types.hpp"

namespace quill {

/// Static trading parameters for one instrument.
struct InstrumentSpec {
    std::string symbol;
    /// Minimum price increment in fixed-point units (100 = 0.01).
    Price tick_size = 100;
    /// Order quantities must be a multiple of this.
    Quantity lot_size = 1;
    Quantity max_order_quantity = 1'000'000;
    /// Dynamic price collar around the last trade price, in basis points. Limit orders
    /// priced outside the band are rejected and market orders will not sweep beyond it.
    /// Zero disables the collar; it is also inactive until the first trade prints.
    std::uint32_t collar_bps = 0;
};

}  // namespace quill
