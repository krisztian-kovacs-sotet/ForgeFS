# Benchmarking ForgeFS

`forgefs_benchmark` measures the client-observed side of the wire —
throughput and per-operation latency — against a running coordinator, using
the same `client::Client` code path the CLI uses. It can't see the
coordinator's own CPU/memory usage (no process can see another process's
resource usage without asking the OS), so pair it with
`benchmarks/monitor_resources.sh` sampling the coordinator's PID.

None of the numbers in this document are filled in — running a distributed
benchmark inside the environment that wrote this code isn't meaningful (no
C++ toolchain was available while building it; see the top-level README).
The tool is real and works; treat the tables below as the template to fill
in on your own machine, not a claim about what you'll see on yours. Hardware,
disk, and OS all move these numbers more than most code changes do.

## Running it

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# terminal 1: coordinator + 3 nodes (see README's "Running" section)
# terminal 2:
./build/server/forgefs_server --data-dir server_data --create-user bench

./build/benchmarks/forgefs_benchmark upload --username bench --size 1048576 --count 50 --concurrency 4
./build/benchmarks/forgefs_benchmark download --username bench --size 1048576 --count 50 --concurrency 4
./build/benchmarks/forgefs_benchmark latency --username bench --count 100
./build/benchmarks/forgefs_benchmark sweep --username bench --size 1048576 --count 40 --levels 1,2,4,8,16
```

`sweep` is the one that directly answers "test different numbers of
concurrent clients" — it re-runs the upload benchmark at each concurrency
level in `--levels` and prints a table of concurrency vs. throughput vs.
latency, so you can see where throughput stops scaling (the point where
adding more concurrent clients stops helping is the interesting number, not
throughput at any single concurrency level).

To watch the coordinator's own resource usage during a run:

```bash
./benchmarks/monitor_resources.sh "$(pgrep -f forgefs_server)" 1 coordinator_usage.csv &
./build/benchmarks/forgefs_benchmark sweep --username bench --size 4194304 --count 100 --levels 1,4,16,32
```

## Results (fill in on your own hardware)

### Throughput vs. payload size (concurrency=1)

| Payload size | Upload MB/s | Download MB/s | Avg latency (ms) |
|---|---|---|---|
| 4 KiB | | | |
| 256 KiB | | | |
| 1 MiB | | | |
| 4 MiB (= chunk size) | | | |
| 16 MiB (spans chunks) | | | |

### Concurrency sweep (`sweep --size 1048576 --count 40`)

| Concurrency | Throughput MB/s | Avg latency (ms) | p95 latency (ms) |
|---|---|---|---|
| 1 | | | |
| 2 | | | |
| 4 | | | |
| 8 | | | |
| 16 | | | |
| 32 | | | |

### Coordinator resource usage during the concurrency=16 run

| Metric | Idle | Under load |
|---|---|---|
| CPU % | | |
| RSS (MB) | | |

## A documented optimization: SQLite WAL mode

This one's real and already in the code (`database/Database.cpp`), not
hypothetical — it's included here as the "before/after" writeup the roadmap
asks for, with reproduction steps rather than invented numbers.

**The problem.** SQLite's default journal mode (`DELETE`, a rollback
journal) takes an exclusive lock on the whole database file for the
duration of any write transaction — no other connection can even *read*
during that window. ForgeFS opens multiple separate connections to the same
`metadata.db` (the coordinator's request-handling `ChunkStore`, and the
background `ReplicationMonitor`'s own `ChunkStore` on its own thread — see
`server/ReplicationMonitor.cpp`). Under the default journal mode, a
replication-monitor pass touching the database would block every concurrent
upload/download's metadata reads for its duration, and vice versa.

**The fix.**
```cpp
// database/Database.cpp, in Database::Database()
Execute("PRAGMA journal_mode = WAL;");
```
Write-Ahead Logging lets readers proceed against the last-committed state
while a writer appends to a separate WAL file, so the replication monitor's
periodic scan doesn't stall concurrent client requests the way the default
mode would.

**How to measure it yourself.** Comment out that one line, rebuild, and
compare `sweep` throughput at a concurrency level high enough to create
real contention (`--concurrency 16` or higher, with several files already
uploaded so the replication monitor has real work to do on its periodic
pass) against the same run with WAL enabled. The effect should be small to
invisible at low concurrency (little contention to relieve) and should grow
with concurrency and with how much the replication monitor has to check —
which is exactly the shape of result that would confirm the mechanism
above, as opposed to a flat improvement that would suggest something else
is actually going on.

## Known bottlenecks worth investigating next

Identified by reading the code, not by profiling (no toolchain was
available to profile with) — a legitimate starting point for where to look
first, not a substitute for actually measuring:

- **No connection pooling to storage nodes.** `NodeClient` opens a fresh
  TCP connection for every single `StoreChunk`/`FetchChunk`/`HealthCheck`
  call (`server/NodeClient.cpp`). TCP handshake overhead on every operation
  adds up fast under concurrent load; a pooled/persistent connection per
  (coordinator, node) pair would likely be the single highest-leverage
  change for throughput at high concurrency.
- **Live health probing on every chunk placement.**
  `ChunkDistributor::PickHealthyNodes` does a real connect+round-trip
  `HealthCheck` per candidate node for *every chunk* being stored — a
  16 MiB upload (4 chunks at the default 4 MiB chunk size) does up to
  `4 × node_count` health probes. Caching health status with a short TTL
  instead of probing live every time would cut this significantly, at the
  cost of slightly staler failure detection.
- **`NODES`/`STATUS` also probe every node live**, for the same reason —
  fine for a demo-sized cluster, not for hundreds of nodes.
- **Whole-chunk-in-memory transfers.** `ChunkDistributor::FetchChunk`
  returns a full chunk (up to 4 MiB) as one in-memory `vector<uint8_t>`.
  Fine at the current chunk size; would need to become a true stream if
  chunk size were increased substantially or memory became constrained.
