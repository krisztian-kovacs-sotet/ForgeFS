# ForgeFS security model

This documents what Phase 8 actually secures, the specific mechanisms used,
and — just as importantly — what's explicitly out of scope. Being precise
about the boundary is the point: a security design that doesn't say what it
*doesn't* cover isn't finished.

## Passwords

- Stored as `crypto::HashPassword()`'s output: PBKDF2-HMAC-SHA256, a random
  16-byte salt per user, 210,000 iterations (OWASP's 2023 minimum
  recommendation for PBKDF2-SHA256). The plaintext password is never
  written to disk, logged, or held longer than the single `VerifyPassword`/
  `HashPassword` call needs it.
- The stored format is self-describing —
  `pbkdf2-sha256$<iterations>$<salt-hex>$<derived-key-hex>` — so the scheme
  or iteration count can change later without invalidating existing hashes;
  `VerifyPassword` reads the parameters back out of the stored string
  instead of assuming current constants.
- Verification uses `CRYPTO_memcmp` (constant-time) rather than `==`, so a
  failed login doesn't leak timing information about how many leading bytes
  of the derived key matched.
- **Not implemented**: password complexity rules, breach-database checks,
  rate limiting or lockout after repeated failed logins. A production
  deployment would want at least the last of these — see "Known gaps"
  below.

## Sessions

- `POST /login` (in ForgeFS's terms, the `kLogin` request) exchanges a
  verified username/password for a 256-bit random bearer token
  (`crypto::GenerateRandomToken`, OpenSSL `RAND_bytes`). Every other client
  request carries that token as the first field of its payload; the
  coordinator looks it up in an in-memory `SessionManager` map and rejects
  the request with "authentication required" if it's missing or unknown.
- Sessions are **in-memory only** — a coordinator restart invalidates every
  token and every client just logs in again. This is a deliberate
  simplification, not an oversight: it avoids needing to reason about
  persisted-session invalidation, at the cost of no session surviving a
  restart.
- Tokens **don't expire** and there's no `logout` request (the roadmap's
  CLI only lists `login`). A leaked token is valid until the coordinator
  process restarts. A production system would add expiry and revocation.
- The CLI caches its token at `$HOME/.forgefs/token` with `0600`
  permissions (owner read/write only) so other local users on a shared
  machine can't read it directly — but the token still grants full access
  to any command run as that user.

## Authorization

Any authenticated user can perform any file operation (list/upload/
download/delete/verify) on any file — there's no per-file ownership or ACL
system. This matches the roadmap's scope: the CLI and protocol have no
concept of "whose file is this," so the authorization boundary implemented
here is *authenticated vs. not*, not *authorized for this specific file*. A
multi-tenant deployment would need to add file ownership and per-operation
checks on top of this.

## Transport encryption (TLS)

- Client↔coordinator traffic can be wrapped in TLS: start the coordinator
  with `--tls-cert` and `--tls-key`, and the client with `--tls` (see
  [README.md](../README.md)). `TcpSocket::UpgradeToTlsServer`/
  `UpgradeToTlsClient` wrap the existing socket with OpenSSL, minimum
  protocol version TLS 1.2.
- Peer verification is **opt-in** via `--ca PATH` on the client. Without
  it, the client accepts any certificate the server presents — appropriate
  only for a self-signed dev/demo certificate that hasn't been distributed
  to clients out of band, and explicitly a man-in-the-middle risk otherwise.
  Generate a self-signed dev cert with:
  ```bash
  openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
      -keyout server.key -out server.crt -subj "/CN=localhost"
  ```
- TLS is **off by default** (plaintext) so the Phase 1–7 workflow keeps
  working without requiring a certificate just to try the demo.
- **Coordinator↔storage-node traffic is not encrypted or authenticated at
  all**, in this phase. See "Known gaps."

## Known gaps (and how they'd be closed)

Being upfront about these is more useful than pretending they don't exist:

- **`kRegisterNode` is unauthenticated.** Any host that can reach the
  coordinator's port can register itself as a storage node and start
  receiving chunk writes — including chunks belonging to other users'
  files. Fix: a pre-shared node-registration secret, or mTLS between
  coordinator and nodes, or restricting the deployment to a private network
  that storage nodes and the coordinator share exclusively (the Docker
  Compose setup in Phase 9 does the latter by default).
- **Node↔node and coordinator↔node traffic is plaintext.** A network-level
  attacker positioned between the coordinator and a storage node can read
  or tamper with chunk bytes and control messages. The same
  `TcpSocket::UpgradeToTls*` machinery used for the client link could be
  reused here; it wasn't, to keep this phase's scope to what the roadmap
  asks for ("TLS for client/server communication").
- **No encryption at rest.** Chunk files on storage nodes and the SQLite
  metadata database are plaintext on disk. Anyone with filesystem access to
  a node reads that node's chunks directly; anyone with access to the
  coordinator's `metadata.db` reads filenames, sizes, and password hashes
  (not plaintext passwords, but still a resource worth protecting).
- **No audit trail beyond basic logging.** The coordinator logs
  `login: <username>` and per-request opcodes, but there's no durable,
  queryable record of "who touched which file when."
- **No rate limiting anywhere** — not on login attempts, not on
  upload/download volume per user.

## Answering the roadmap's own interview questions

- *How are users authenticated and authorized?* Username/password → PBKDF2
  verification → random bearer token → token required on every subsequent
  request. Authorization is binary (authenticated or not), not per-file.
- *Where are encryption keys stored?* There's no long-lived symmetric key
  to store: TLS negotiates ephemeral per-connection session keys, and
  password verification uses a per-user random salt stored alongside its
  hash in `metadata.db` (a salt isn't a secret — it just needs to be
  unique, which `RAND_bytes` guarantees with overwhelming probability).
- *What's the biggest security gap you'd fix first at scale?* The
  unauthenticated node protocol — it's the difference between "a
  compromised client account" and "a compromised client account or any
  host on the network."
