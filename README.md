# quill-match

A limit order book and matching engine in C++20, built around the parts of an exchange
core that are easy to describe and hard to get exactly right: price-time priority, the
full set of common order types, cancel/replace semantics, self-trade prevention, an
order-level market data feed that consumers can rebuild the book from, and a FIX-style
order-entry gateway. It comes with a lock-free ingress queue, a latency histogram, a CLI
for driving it, a randomized differential test suite, and Google Benchmark
micro-benchmarks.

## Features

**Matching**
- Central limit order book per instrument with strict price-time priority.
- Order types: limit, market, immediate-or-cancel, fill-or-kill, post-only and iceberg
  (displayed peak plus reserve; each refill loses time priority).
- Cancel and cancel/replace. A pure size reduction at the same price keeps queue
  position; any price change or size increase re-queues and can trade immediately.
  Replace quantities are total quantities (FIX `OrderQty` semantics), so already-executed
  quantity is accounted for.
- Self-trade prevention per order: cancel incoming, cancel resting, or cancel both.
- Dynamic price collar around the last trade: limit orders outside the band are rejected
  and market orders stop sweeping at the band.
- Tick size, lot size and maximum order size validation per instrument.
- Multiple instruments behind one engine with global order and match id sequences.

**Infrastructure**
- `SpscQueue`: bounded lock-free single-producer/single-consumer ring with cache-line
  separated indices and cached opposite-side indices.
- `EngineRunner`: runs the engine on a dedicated (optionally pinned) thread fed by the
  SPSC queue and records enqueue-to-matched latency.
- `LatencyHistogram`: log-linear (HdrHistogram-style) histogram covering the full 64-bit
  range with bounded relative error, percentiles, merge and formatted output.
- `ObjectPool` slab allocator for orders and levels, and an open-addressing
  `FlatHashMap` with backward-shift deletion for the order id index.

**Market data and connectivity**
- ITCH-style binary feed publisher (add, execute, cancel, delete, replace, stock
  directory and system event messages; NASDAQ ITCH 5.0 layouts and two-byte length
  framing) and a streaming decoder that tolerates frames split across reads.
- `BookBuilder`: a feed consumer that rebuilds order-level books from the feed.
- FIX 4.4-style tag=value gateway: Logon, Heartbeat, TestRequest, Logout,
  NewOrderSingle, OrderCancelRequest and OrderCancelReplaceRequest in; ExecutionReport,
  OrderCancelReject and session Reject out. BodyLength and CheckSum are validated and
  generated, sequence numbers are enforced, and ClOrdID chains are tracked across
  replaces.

**Tooling**
- `quill simulate`: drives the engine with deterministic synthetic order flow,
  synchronously or through the SPSC queue (optionally paced), and prints latency
  percentiles. Can write the resulting ITCH feed to a file.
- `quill replay`: rebuilds books from an ITCH file and prints depth and statistics.
- `quill fix`: an interactive or scripted FIX session over stdin/stdout.

## Architecture

```
               FIX text                               synthetic flow (sim)
                  |                                           |
           +--------------+                         +------------------+
           | fix::Gateway |                         | EngineRunner     |
           |  sessions,   |                         |  producer thread |
           |  ClOrdID map |                         |  SpscQueue<Cmd>  |
           +------+-------+                         +--------+---------+
                  |  NewOrder / Cancel / Replace             |
                  v                                          v
           +------------------------------------------------------------+
           | MatchingEngine: instrument routing, order/match ids, clock |
           |   OrderBook (one per instrument)                           |
           |     PriceLadder bids / asks  -> PriceLevel FIFO -> Order   |
           |     FlatHashMap<OrderId, Order*>, ObjectPool<Order>        |
           +-----------------------------+------------------------------+
                                         | EventListener callbacks
                       +-----------------+------------------+
                       v                 v                  v
               execution reports   itch::Publisher     your listener
               (back to Gateway)         |
                                         v  ITCH frames
                                  itch::BookBuilder / quill replay
```

Design notes:

- **Price ladder.** Each side keeps its non-empty levels in a sorted `std::vector`
  ordered from worst to best, so the touch is `back()`. New liquidity overwhelmingly
  arrives at or near the touch, where inserting or erasing moves very few pointers, and
  arbitrary-price lookups are a binary search over contiguous memory. Asks are ranked by
  negated price so both sides share one code path.
- **Intrusive orders.** Orders are doubly-linked list nodes that point at their level,
  so cancel is O(1) after the hash lookup. Levels track both displayed and total
  (displayed plus iceberg reserve) quantity.
- **Events, not return values.** The book reports everything through an
  `EventListener`: a private stream (accepted, rejected, trade, cancelled, replaced) and a
  public order-level market data stream (add, execute, reduce, delete). Market data
  never reveals iceberg reserve; each refill is published as a fresh add, which is how
  the feed stays reconstructible.
- **Deterministic time.** Timestamps come from an injected `Clock`; tests use a manual
  clock, the CLI uses the monotonic clock.
- **Fixed-point prices.** Prices are `int64` with four implied decimals, the same scale
  ITCH uses, so the feed needs no conversion and no floating point touches matching.

## Building

Requirements: a C++20 compiler (developed with GCC 13), CMake 3.20 or newer, and network
access on first configure (GoogleTest and Google Benchmark are fetched with
`FetchContent`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake options:

| Option                   | Default | Meaning                                         |
|--------------------------|---------|-------------------------------------------------|
| `QUILL_BUILD_TESTS`      | ON      | Build the GoogleTest suite                      |
| `QUILL_BUILD_BENCHMARKS` | ON      | Build the Google Benchmark suite                |
| `QUILL_BUILD_APPS`       | ON      | Build the `quill` CLI                           |
| `QUILL_WERROR`           | ON      | Warnings are errors                             |
| `QUILL_SANITIZE`         | OFF     | AddressSanitizer + UndefinedBehaviorSanitizer   |
| `QUILL_TSAN`             | OFF     | ThreadSanitizer                                 |
| `QUILL_NATIVE`           | OFF     | `-march=native`                                 |

## Running the tests

```sh
ctest --test-dir build --output-on-failure
# or run the binary directly
./build/tests/quill_tests
```

The suite has 209 tests. Beyond the unit tests for each component, two checks carry most
of the weight:

- **Differential model test** (`tests/book_model_test.cpp`). A deliberately naive
  reference book (`tests/reference_book.hpp`: one flat vector, linear scans, explicit
  sequence numbers, fill-or-kill checked by trial execution on a copy) is driven with the
  same random stream of limit, market, IOC, FOK, post-only and iceberg orders, cancels,
  replaces and self-trade-prevention modes as the real engine. After every command the
  fills, accept/reject outcomes and the full queue on both sides (ids, prices, displayed
  and reserve quantity) must match, and the book's internal invariants are checked.
  Eight seeds, 3,000 commands each.
- **Feed reconstruction test** (`tests/itch_feed_test.cpp`). The engine runs 40,000
  simulated commands across three instruments while publishing ITCH; decoding the feed
  with `BookBuilder` must reproduce the engine's displayed depth level for level, with no
  anomalies, the same live order count and the same traded volume.

The suite is also clean under AddressSanitizer + UBSan (`-DQUILL_SANITIZE=ON`) and under
ThreadSanitizer (`-DQUILL_TSAN=ON`; on recent kernels run it with
`setarch "$(uname -m)" -R ./build/tests/quill_tests`).

## Using the CLI

```sh
# One million measured commands over four instruments, matching on this thread.
./build/apps/quill simulate --orders 1000000

# Same flow through the SPSC queue to a matching thread, paced at 500k commands/s.
./build/apps/quill simulate --threaded --rate 500000

# Write the market data feed, then rebuild the books from it.
./build/apps/quill simulate --orders 200000 --itch-out feed.itch
./build/apps/quill replay feed.itch --depth 5 --symbol SYM1

# A scripted FIX session ('|' delimiter, lenient envelope).
./build/apps/quill fix < examples/fix_session.txt
```

Example FIX exchange (one ExecutionReport wrapped over two lines; BodyLength and CheckSum
cover the SendingTime field, so they change from run to run):

```
in : 8=FIX.4.4|35=D|49=BOB|56=QUILL|34=2|11=B-1|55=AAPL|54=1|38=250|40=2|44=189.60|59=3|
out: 8=FIX.4.4|9=155|35=8|49=QUILL|56=BOB|34=3|52=20261010-04:34:05.949|37=3|11=B-1|17=E4|
     150=F|39=1|55=AAPL|54=1|38=250|40=2|44=189.60|31=189.50|32=100|151=150|14=100|6=189.5|10=223|
```

Gateway tag support: `11` ClOrdID, `41` OrigClOrdID, `55` Symbol, `54` Side (1/2),
`38` OrderQty, `40` OrdType (1 market, 2 limit), `44` Price, `59` TimeInForce
(0 and 1 rest, 3 IOC, 4 FOK), `18` ExecInst containing `6` for post-only, `1138`
DisplayQty for icebergs, `2964` SelfMatchPreventionInstruction (1 cancel incoming,
2 cancel resting, 3 both).

## ITCH wire format

All integers big-endian; every message starts with type (1), stock locate (2), tracking
number (2) and nanoseconds since midnight (6). Files are a sequence of two-byte length
prefixed frames.

| Type | Message           | Size | Body                                                  |
|------|-------------------|------|-------------------------------------------------------|
| `S`  | System event      | 12   | event code                                            |
| `R`  | Stock directory   | 27   | symbol (8), round lot (4), tick size (4)              |
| `A`  | Add order         | 36   | order ref (8), side (1), shares (4), symbol (8), price (4) |
| `E`  | Order executed    | 31   | order ref (8), shares (4), match number (8)           |
| `X`  | Order cancel      | 23   | order ref (8), cancelled shares (4)                   |
| `D`  | Order delete      | 19   | order ref (8)                                         |
| `U`  | Order replace     | 35   | original ref (8), new ref (8), shares (4), price (4)  |

`A`, `E`, `X`, `D` and `U` match the ITCH 5.0 layouts. The stock directory is a reduced
variant. The publisher expresses a priority-losing replace as delete plus add, and the
decoder and book builder also accept `U`.

## Benchmarks

Measured on the development machine: AMD Ryzen 7 7800X3D, 14 logical CPUs visible to
WSL2 (Linux 6.18), GCC 13.3, `-O3`, no `-march=native`. **Caveat:** the machine was
heavily loaded by unrelated work during every run (load average 29 to 38 on 14 CPUs), so
wall-clock times, tails and anything cross-thread are noisier and slower than they would
be on a quiet box. Single-threaded rows below quote CPU time, which is far less affected.
Reproduce with:

```sh
./build/bench/quill_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Medians of 5 repetitions:

| Benchmark | CPU time | Throughput |
|-----------|---------:|-----------:|
| Add + cancel passive order, 10 levels per side | 61.6 ns / pair | 32.5M ops/s |
| Add + cancel passive order, 1,000 levels per side | 88.0 ns / pair | 22.7M ops/s |
| Add + cancel at a random depth (new levels mid-ladder) | 77.9 ns / pair | 25.7M ops/s |
| Aggressive order taking 1 resting order (incl. re-adding it) | 101 ns | 19.8M ops/s |
| Aggressive order taking 10 resting orders | 488 ns | 22.5M ops/s |
| Aggressive order taking 50 resting orders | 2,146 ns | 23.8M ops/s |
| Replace, size down in place | 14.4 ns | 69.2M ops/s |
| Replace, re-price (re-queue) | 53.4 ns | 18.7M ops/s |
| Iceberg refill (take a peak, re-queue) | 114 ns / 2 ops | 17.5M ops/s |
| Mixed simulated flow, 200k commands, 4 instruments | 14.2 ms / batch | 14.1M commands/s |
| ITCH encode one add-order frame | 12.1 ns | 82.6M msgs/s |
| ITCH decode only, feed from 200k simulated commands (215,899 msgs, 7.0 MB) | 1.33 ms / feed | 162M msgs/s, 4.9 GiB/s |
| ITCH decode and rebuild books, same feed | 10.9 ms / feed | 19.7M msgs/s |
| FIX parse NewOrderSingle (strict envelope checks) | 286 ns | 3.5M msgs/s |
| FIX build ExecutionReport | 420 ns | 2.4M msgs/s |
| Latency histogram record | 21.0 ns | 47.5M ops/s |

Cross-thread queue throughput, 1M items per run, wall time (most affected by the load):

| Queue | Items/s |
|-------|--------:|
| `SpscQueue<uint64_t>`, capacity 1,024 | 104M |
| `SpscQueue<uint64_t>`, capacity 65,536 | 126M |
| `SpscQueue<Command>` (80-byte commands) | 21.6M |
| `std::mutex` + `std::deque<uint64_t>` baseline | 8.1M |

The id index is not a win in this setup: `FlatHashMap` lookups took 5.5 ns versus
2.1 ns for `std::unordered_map` with 1,000 keys and 10.2 ns versus 15.1 ns with
1,000,000 keys, and insert/erase churn took 53 ns versus 26 ns. libstdc++ hashes
integers with the identity function, which turns sequential order ids into perfectly
sequential buckets. `FlatHashMap` deliberately mixes keys (SplitMix64 finalizer) so that
client-assigned ids with regular strides cannot build long probe chains, and that
robustness costs locality on the sequential-id fast path.

End-to-end runs of the CLI (2,000,000 measured commands after 200,000 warm-up, 4
instruments, seed 1, same loaded machine):

| Mode | Throughput | p50 | p90 | p99 | p99.9 |
|------|-----------:|----:|----:|----:|------:|
| `simulate` (matching only, per command) | 4.54M/s | 120 ns | 220 ns | 491 ns | 1,303 ns |
| `simulate --threaded --rate 500000` (enqueue to matched) | 499K/s | 299 ns | 6.8 us | 2.9 ms | 5.1 ms |

The per-command numbers include two `steady_clock` reads (about 10 ns floor here). The
threaded tail is dominated by the matching thread being descheduled on a saturated
machine: once it loses its CPU for a few milliseconds, every queued command inherits that
delay. Unpaced threaded runs simply fill the 65,536-slot queue and measure queueing time,
which is why `--rate` exists.

## Repository layout

```
include/quill/        public headers (engine, book, containers, queue, histogram)
include/quill/itch/   feed messages, codec, publisher, book builder
include/quill/fix/    FIX message parsing/building and the gateway
include/quill/sim/    synthetic order flow generator
src/                  library implementation
apps/cli/             the quill command line tool
tests/                GoogleTest suite, reference model
bench/                Google Benchmark suite
examples/             scripted FIX session
```

## Roadmap

Things that are deliberately unfinished:

- FIX session recovery: there is no ResendRequest/SequenceReset handling, so any
  sequence gap ends the session, and there is no persistence of outbound messages.
- No cancel-on-disconnect: orders survive a logout and can be managed after logging back
  on.
- No network transport. The gateway consumes and produces complete messages; the CLI
  wires it to stdin/stdout. A TCP acceptor (and MoldUDP64 framing for the feed) would be
  the next step.
- Day orders are treated as good-till-cancel because the engine has no trading-session
  calendar, auctions or market states.
- Market data has no snapshot or recovery channel; a late joiner sees anomalies until the
  orders it missed are gone.
- The order id index could use an identity or Fibonacci hash when ids are known to be
  engine-assigned, recovering the locality `std::unordered_map` gets today.
- Only GCC is exercised in CI; the code is standard C++20 but has not been built with
  Clang or MSVC yet.

## License

MIT, see [LICENSE](LICENSE).
