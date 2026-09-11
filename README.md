# ForgeFS

**Secure Distributed File Storage System — written in modern C++.**

ForgeFS is a distributed file-storage system: a client uploads files to a
server, the server splits files into chunks, distributes those chunks across
storage nodes, verifies integrity with SHA-256, and keeps replicas so files
stay available when a node fails.

## Target demo

1. Start the system with Docker Compose.
2. Upload a file through the `forgefs` command-line client.
3. Watch the file get split and distributed across several storage nodes.
4. Stop one storage node.
5. Download the file successfully from the remaining replicas.
6. Verify the downloaded file with SHA-256.

## Technology stack

| Technology            | Use                                        |
|------------------------|---------------------------------------------|
| C++20                  | Main application language                  |
| CMake                  | Build system                               |
| Linux                  | Development / runtime environment          |
| TCP sockets            | Client/server and node communication       |
| SQLite                 | Metadata and file information              |
| OpenSSL                | TLS and cryptographic functionality        |
| SHA-256                | File/chunk integrity verification          |
| GoogleTest             | Unit tests                                 |
| Docker + Docker Compose| Run server and multiple storage nodes      |

## Project structure

```
ForgeFS/
├── client/       CLI client
├── server/       Coordinator server
├── storage/      Storage node
├── network/      TCP socket wrapper
├── protocol/     Wire protocol
├── crypto/       SHA-256 / password hashing
├── database/     SQLite metadata layer
├── common/       Shared utilities
├── tests/        GoogleTest unit/integration tests
├── benchmarks/   Performance benchmarking tool
├── docker/       Dockerfiles
├── docs/         Architecture, protocol, security, benchmark docs
├── CMakeLists.txt
├── docker-compose.yml
└── README.md
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — components, request flows
  (upload/download/node-failure), data model, concurrency model, and the
  tradeoffs deliberately made (single coordinator, unauthenticated node
  protocol, no rebalancing) versus what a production system would add.
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — the wire protocol: framing,
  opcodes, streaming, and the exact request/response sequence for every
  operation.
- [docs/SECURITY.md](docs/SECURITY.md) — the threat model: what's actually
  secured (password hashing, sessions, TLS) and what's explicitly out of
  scope, with why.
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) — how to run `forgefs_benchmark`,
  a results-table template, a documented optimization with reproduction
  steps, and known bottlenecks worth profiling next.

## Building

Requires a C++20 compiler, CMake 3.20+, OpenSSL and SQLite3 development
headers. Sockets/TLS code targets POSIX/Linux — build natively on Linux, in
WSL, or skip building entirely and use the Docker setup below.

From a fresh Ubuntu/Debian install:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev libsqlite3-dev git

git clone <this-repo-url> ForgeFS
cd ForgeFS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

(`FORGEFS_BUILD_TESTS=ON` by default also fetches GoogleTest via CMake's
`FetchContent` on first configure — that step needs network access once;
pass `-DFORGEFS_BUILD_TESTS=OFF` to skip it and build only the binaries.)

The coordinator binary is built at `build/server/forgefs_server`, the
storage-node binary at `build/storage/forgefs_storage`, and the client at
`build/client/forgefs_client`.

## Running

```bash
# terminal 1: coordinator
./build/server/forgefs_server --port 9000 --data-dir server_data

# terminal 2, 3 & 4: storage nodes, each registering itself with the coordinator
./build/storage/forgefs_storage --port 9101 --data-dir node1_data --id node-1 \
    --advertise-host 127.0.0.1 --register 127.0.0.1:9000
./build/storage/forgefs_storage --port 9102 --data-dir node2_data --id node-2 \
    --advertise-host 127.0.0.1 --register 127.0.0.1:9000
./build/storage/forgefs_storage --port 9103 --data-dir node3_data --id node-3 \
    --advertise-host 127.0.0.1 --register 127.0.0.1:9000

# once, to provision an account (prompts for a password)
./build/server/forgefs_server --data-dir server_data --create-user alice

# terminal 5: client — login once, then every other command reuses the
# cached token at $HOME/.forgefs/token
./build/client/forgefs_client login alice
./build/client/forgefs_client nodes
./build/client/forgefs_client upload ./some_file.txt
./build/client/forgefs_client list
./build/client/forgefs_client download some_file.txt ./downloaded_file.txt
./build/client/forgefs_client verify some_file.txt
./build/client/forgefs_client status
./build/client/forgefs_client delete some_file.txt
```

Upload/download stream the file in 64 KiB pieces over the wire and are
SHA-256 verified end-to-end: the client prints the digest it computed and
confirmed with the server, and a corrupted/truncated transfer is rejected
rather than silently accepted.

The coordinator (`forgefs_server`) never stores chunk bytes itself — it
splits each upload into fixed-size (4 MiB by default) chunks and replicates
each one onto up to 3 registered storage nodes (`forgefs_storage`) over the
network, round-robin, tracking every chunk's holder(s) in
`<data-dir>/metadata.db` (SQLite; degrades gracefully to fewer copies if
fewer than 3 nodes are healthy). Downloads fetch each chunk from whichever
replica responds first, failing over to the next if one is down; `verify`
does the same and compares the recomputed hash against what was recorded at
upload time, so it also catches storage-side corruption, not just transfer
corruption.

A storage node registers itself once at startup via
`--register coordinator_host:port`. A background replication monitor on the
coordinator periodically checks every chunk's live replica count and
re-replicates any that dropped below 3 (e.g. because a node went down) onto
other healthy nodes — so killing a storage node process doesn't just fail
over reads, it self-heals the missing copies once the coordinator notices.

Both the coordinator and each storage node accept connections onto a
thread pool (`--threads N`, default 8) rather than serving one at a time, so
multiple clients can upload/download/list concurrently. Shared state is
protected accordingly: the SQLite metadata store takes an internal mutex
around each (short, local) operation without holding it across any network
call, and node registration uses a lock-protected map — so concurrent
requests don't serialize on network-bound work, only on the brief metadata
updates.

The client defaults to `127.0.0.1:9000`; override with `--server host:port`
or the `FORGEFS_SERVER` environment variable.

## Authentication and TLS

Every command except `login` requires a session: `forgefs login <username>`
prompts for a password (hidden input), exchanges it for a bearer token, and
caches that token at `$HOME/.forgefs/token`. Passwords are stored server-side
as PBKDF2-HMAC-SHA256 with a random per-user salt — see
[docs/SECURITY.md](docs/SECURITY.md) for the full model, including what's
deliberately out of scope.

Accounts are provisioned from the coordinator's own command line (there's no
network signup endpoint):

```bash
./build/server/forgefs_server --data-dir server_data --create-user alice
```

TLS between client and coordinator is opt-in:

```bash
# generate a self-signed dev certificate once
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -keyout server.key -out server.crt -subj "/CN=localhost"

./build/server/forgefs_server --data-dir server_data \
    --tls-cert server.crt --tls-key server.key

./build/client/forgefs_client --tls login alice
```

Coordinator↔storage-node traffic is not TLS-wrapped in this phase — see
docs/SECURITY.md for why and what that implies for deployment.

## Running with Docker Compose

This is the target demo: one command brings up a coordinator and three
storage nodes, each node registering itself automatically, with a demo
account provisioned on first boot.

```bash
docker compose up --build -d
docker compose logs -f coordinator   # watch nodes register
```

The CLI client isn't started automatically (it's a one-shot command, not a
long-running service — see the `cli` profile in docker-compose.yml). Drop
files to upload into `./demo_files` on the host; it's mounted at `/files` in
the client container:

```bash
docker compose run --rm client login demo        # password from FORGEFS_PASSWORD
docker compose run --rm client nodes
cp ./some_file.txt ./demo_files/
docker compose run --rm client upload /files/some_file.txt
docker compose run --rm client list

docker compose stop storage-2                    # kill a replica
docker compose run --rm client download some_file.txt /files/downloaded.txt
docker compose run --rm client verify some_file.txt

docker compose down -v                           # tear down, including data volumes
```

Configuration is entirely environment-variable driven in the compose file
(`FORGEFS_PORT`, `FORGEFS_DATA_DIR`, `FORGEFS_ID`, `FORGEFS_ADVERTISE_HOST`,
`FORGEFS_REGISTER`, `FORGEFS_BOOTSTRAP_USER`/`_PASSWORD`, ...) — every CLI
flag documented above has an equivalent env var, and a flag wins if both are
set. Storage nodes retry registration for up to a minute on startup, since
Compose doesn't guarantee the coordinator is ready first.

The demo credentials in `docker-compose.yml` (`demo` / `demo-password`) are
exactly that — demo credentials, baked into a file meant to be read. Don't
reuse this pattern for anything real; see docs/SECURITY.md.

## Testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`FORGEFS_BUILD_TESTS` (default `ON`) fetches GoogleTest via CMake's
`FetchContent` on first configure (needs network access once; cached under
`build/_deps` after) and builds two executables:

- **`forgefs_unit_tests`** — protocol payload encode/decode, SHA-256 known
  vectors, PBKDF2 password hashing, hex codec, session-token issuance, path
  traversal rejection, and `ChunkStore`'s metadata bookkeeping (re-upload
  replacing a file's chunks, shared-chunk reference counting, delete
  orphaning) — all against real SQLite, in an isolated temp DB per test.
- **`forgefs_integration_tests`** — spins up a real coordinator and three
  real storage nodes as subprocesses (`tests/integration/TestHarness`) and
  drives them over the actual wire protocol via the same `client::Client`
  code the CLI uses: upload/list/download round-trips, auth rejection
  (missing/wrong credentials), delete, and — closer to what the roadmap
  actually asks be tested — **killing a storage node mid-cluster and
  confirming download still succeeds from a surviving replica**,
  **corrupting a replica's chunk files on disk directly and confirming the
  coordinator detects the mismatch and fails over to a good copy** (this is
  what caught a real gap: `ChunkDistributor::FetchChunk` didn't verify a
  fetched chunk's hash before Phase 10 added the test that exposed it), and
  **sending garbage bytes over a raw socket and confirming the coordinator
  drops that connection without crashing or otherwise affecting other
  clients**.

## Benchmarking

```bash
./build/benchmarks/forgefs_benchmark sweep --username bench --size 1048576 --count 40 --levels 1,2,4,8,16
```

`forgefs_benchmark` measures upload/download throughput and latency
(min/avg/p50/p95/p99) at one or more concurrency levels against a running
coordinator; `benchmarks/monitor_resources.sh` samples the coordinator
process's own CPU/RSS via `/proc` to run alongside it. See
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) for full usage, a results-table
template, a documented optimization (SQLite WAL mode) with reproduction
steps, and a few known bottlenecks worth profiling next (no connection
pooling to storage nodes; live health-probing on every chunk placement).

## Demo recording checklist

No screenshots or demo video are checked in — they need an actual running
system to capture honestly, and this repository was built in an environment
without a C++ toolchain to compile and run it in (see the note at the
bottom of this section). Once you've built and run it yourself:

- [ ] Terminal recording (asciinema, or a plain screen recording) of the
      target demo end to end: `docker compose up --build -d`, `nodes`,
      `upload`, `list`, stop a storage node, `download` succeeding anyway,
      `verify`.
- [ ] A screenshot of `forgefs nodes` / `forgefs status` output showing a
      healthy 3-node cluster.
- [ ] A screenshot or clip of the coordinator's logs during a node failure
      and the replication monitor's recovery.
- [ ] 3–5 minute walkthrough video covering: what it is, the architecture
      diagram in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), the live
      demo above, and one design decision you'd defend in an interview
      (e.g. why chunks are content-addressed, or why the node protocol
      isn't authenticated yet).

Drop the recordings in `docs/media/` and link them from here once captured.

> This codebase was generated by Claude in a Windows sandbox with no C++
> compiler, CMake, or Docker installed — every design decision above was
> made by writing and reasoning about the code, not by running it. Build
> and run it yourself before trusting it; that's not a formality here, it's
> the one verification step that hasn't happened yet.

## Status

Built in stages, per the project roadmap:

- [x] Phase 1 — Project foundation
- [x] Phase 2 — Basic client/server
- [x] Phase 3 — File transfer
- [x] Phase 4 — File chunking
- [x] Phase 5 — Storage nodes
- [x] Phase 6 — Replication
- [x] Phase 7 — Concurrency
- [x] Phase 8 — Security
- [x] Phase 9 — Docker deployment
- [x] Phase 10 — Testing and reliability
- [x] Phase 11 — Performance benchmarking
- [x] Phase 12 — Portfolio polish

Remaining before this is truly "done, done": build it, work through the demo
recording checklist above, and once you're satisfied it behaves as
documented, tag a release —
```bash
git tag -a v1.0.0 -m "ForgeFS v1.0.0"
git push origin v1.0.0
```
— intentionally not run here, since tagging a release is a claim that the
code has been built and verified, which is a step only you can actually do.

## CV / resume description

**ForgeFS — Secure Distributed File Storage System**
Developed a distributed file-storage system in modern C++ (C++20) featuring
a custom binary TCP protocol, concurrent client/node handling via a thread
pool, content-addressed file chunking with 3x replication and automatic
failure recovery, end-to-end SHA-256 integrity verification, PBKDF2
authentication with TLS-secured client communication, Docker Compose
deployment, GoogleTest unit and multi-process integration tests, and a
throughput/latency benchmarking tool with a documented performance
optimization.

## License

MIT — see [LICENSE](LICENSE).
