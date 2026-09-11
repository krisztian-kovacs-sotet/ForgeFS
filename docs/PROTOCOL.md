# ForgeFS wire protocol

A custom binary protocol over plain TCP (optionally TLS-wrapped between
client and coordinator — see [SECURITY.md](SECURITY.md)), rather than an
existing RPC framework, so the message framing, backpressure, and streaming
behavior are all something this project actually built rather than
configured. The canonical definition is [`protocol/Protocol.hpp`](../protocol/Protocol.hpp);
this document is the narrative version.

## Message framing

Every message is a 16-byte header followed by a payload:

```
 0        4  5        6  7  8                              16
 +--------+--+--------+--+------------------------------+
 | magic  |ve| opcode |rr| payload_length (uint64, BE)   |
 +--------+--+--------+--+------------------------------+
```

- `magic` — 4 bytes, ASCII `"FGFS"` (`0x46474653`). A connection that
  doesn't start with this is rejected outright — cheap protection against
  accidentally talking to the wrong service on a misconfigured port.
- `ve` — protocol version (currently `1`).
- `opcode` — 1 byte (see the table below).
- `rr` — 2 reserved bytes, currently always zero.
- `payload_length` — 8 bytes, big-endian, capped at 512 MiB per message
  (`kMaxPayloadBytes`) as a sanity limit — arbitrarily large *files* still
  work because they're split across many messages (see "Streaming" below),
  not because any single message can be unbounded.

All multi-byte integers in payloads are big-endian; strings are a `uint32`
length prefix (big-endian) followed by raw UTF-8 bytes, no null terminator.

## Opcodes

| Value | Name | Direction |
|---|---|---|
| 1 | `LOGIN` | client → coordinator |
| 2 | `LIST` | client → coordinator |
| 3 | `UPLOAD` | client → coordinator |
| 4 | `DOWNLOAD` | client → coordinator |
| 5 | `DELETE` | client → coordinator |
| 6 | `STATUS` | client → coordinator |
| 7 | `NODES` | client → coordinator |
| 8 | `VERIFY` | client → coordinator |
| 10 | `REGISTER_NODE` | storage node → coordinator |
| 11 | `HEALTH_CHECK` | coordinator → storage node |
| 12 | `STORE_CHUNK` | coordinator → storage node |
| 13 | `FETCH_CHUNK` | coordinator → storage node |
| 14 | `DELETE_CHUNK` | coordinator → storage node |
| 100 | `OK` | response |
| 101 | `ERROR` | response (payload: one string, the message) |
| 102 | `DATA` | response carrying a structured payload (e.g. a file listing) |

Numbers are appended, never renumbered, as the protocol grows — a wire
capture from an older build stays interpretable.

## Streaming (UPLOAD / DOWNLOAD)

File bytes move as a run of `DATA` messages (64 KiB each, `kTransferChunkSize`)
rather than one message per file, so memory use on both ends is bounded by
chunk size regardless of file size. Each side computes SHA-256 incrementally
as bytes pass through it and exchanges the digest in a trailer message
afterward — no second read pass over the data just to learn its hash.

```
UPLOAD:   C→S  UPLOAD(token, name, total_size)
          S→C  OK (ready)                | ERROR
          C→S  DATA chunk × N            (until total_size bytes sent)
          C→S  OK (trailer: sha256 hex)
          S→C  OK (confirmed, hashes matched) | ERROR (mismatch / short)

DOWNLOAD: C→S  DOWNLOAD(token, name)
          S→C  OK(total_size)            | ERROR (not found)
          S→C  DATA chunk × N            (until total_size bytes sent)
          S→C  OK (trailer: sha256 hex)
```

Server-side, those 64 KiB wire pieces don't correspond 1:1 to the 4 MiB
storage chunks a file is split into on disk — `UploadSession` re-buffers
incoming wire pieces into storage-sized chunks before handing them to
`ChunkDistributor`, and `Server::HandleDownload` does the reverse, so the
wire chunk size and the storage chunk size are independent knobs.

## Other client requests

```
LOGIN:    C→S  LOGIN(username, password)
          S→C  OK(token)                 | ERROR (invalid credentials)

LIST:     C→S  LIST(token)
          S→C  DATA(count, [name, size]...)

DELETE:   C→S  DELETE(token, name)
          S→C  OK                        | ERROR (not found)

VERIFY:   C→S  VERIFY(token, name)
          S→C  OK(matches: u8, computed sha256 hex, expected sha256 hex)
                                          | ERROR (not found)

STATUS:   C→S  STATUS(token)
          S→C  DATA(node_count, healthy_node_count, file_count, total_bytes)

NODES:    C→S  NODES(token)
          S→C  DATA(count, [id, host, port, healthy: u8]...)
```

Every request here except `LOGIN` carries a session token as the first
payload field; the coordinator validates it against `SessionManager` before
doing anything else and responds `ERROR("authentication required...")`
immediately if it's missing or unknown (see [SECURITY.md](SECURITY.md)).

## Node protocol

A separate trust domain from the client requests above (see SECURITY.md for
why that matters) — the coordinator acts as the client here, storage nodes
as the server:

```
REGISTER: node→S  REGISTER_NODE(node_id, host, port)
          S→node  OK                     | ERROR

HEALTH:   S→node  HEALTH_CHECK
          node→S  OK

STORE:    S→node  STORE_CHUNK(chunk_id, chunk bytes)
          node→S  OK                     | ERROR

FETCH:    S→node  FETCH_CHUNK(chunk_id)
          node→S  OK(chunk bytes)        | ERROR (not found)

DELETE:   S→node  DELETE_CHUNK(chunk_id)
          node→S  OK                     | ERROR
```

A chunk (≤ 4 MiB by default) is small enough to send as a single message
payload — no streaming needed for node traffic the way there is for
arbitrarily large client file transfers.

## Error handling

Any request can get back `ERROR` instead of the response shown above; the
payload is always a single string with a human-readable reason. A
connection that sends a malformed header, an unrecognized magic number, an
oversized payload length, or simply disconnects mid-message is dropped by
the receiving side (`ProtocolError`/`ConnectionClosed`, see
`protocol/Protocol.hpp`) without taking down the process — see the
"garbage bytes" integration test in [`tests/integration/ClusterTest.cpp`](../tests/integration/ClusterTest.cpp)
for a concrete example of that being exercised.
