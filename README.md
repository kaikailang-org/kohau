# kohau

DB clients for [kaikai](https://github.com/kaikailang-org/kaikai). The
persistence substrate that sits between the language's effects and
the DDD vocabulary of [henua](https://github.com/kaikailang-org/henua).

> **Status:** two drivers ship, each in two layers — a low-level
> surface over typed FFI handles (`kohau.sqlite`, `kohau.postgres`)
> and a cell-wrapped ergonomic client (`kohau.sqlite.client`,
> `kohau.postgres.client`) whose ahu cell owns the connection
> lifecycle and seals `Ffi` from callers. Both give request/reply
> query execution with the FFI confined to one fiber. SQLite adds a
> prepared-statement cache and chunked cursors; Postgres adds
> opt-in reconnection. A connection pool is a follow-up on both, as
> are the cache and cursors on the Postgres side.

## What ships

**`kohau.sqlite`** — low-level SQLite client. One-to-one mapping
to libsqlite3's C API, bridged through a thin C shim
(`c/sqlite_shim.{c,h}`) that flattens shapes kaikai's FFI cannot
express (out-parameters, defaulted destructor/length arguments).

Surface (every decl marked `#[unstable]` for the Hanga Roa
edition):

- **Handles**: `Db` (connection), `Stmt` (prepared statement) —
  nominal types, so a connection cannot be confused with a
  statement or a plain `Int`.
- **Lifecycle**: `open(path) : Option[Db]`, `close(db)`.
- **Single-shot**: `exec(db, sql)` for DDL / BEGIN / COMMIT.
- **Prepared statements**: `prepare(db, sql) : Option[Stmt]`,
  `bind_text`, `bind_int`, `step`, `column_int`, `column_text`,
  `column_count`, `reset`, `finalize`.
- **Diagnostics**: `last_insert_rowid`, `changes`, `errmsg`.

The constructors return `Option` — open/prepare failure is a typed
absence, not a `0` sentinel. Status codes follow libsqlite3
convention: `0` is `SQLITE_OK`, `100` is `SQLITE_ROW`, `101` is
`SQLITE_DONE`. (The handles wrap the shim's pointer-as-`Int`
representation in nominal records rather than `extern "C" opaque`
types: an opaque extern type cannot appear in `pub` signatures
today — see `docs/known-regressions.md`. The wrapper is the
representation, not the contract.)

This surface is **usable directly** for scripts, fixtures, and
one-shot tooling that does not need connection ownership. The
cell-wrapped client below is what downstream layers (henua) build
on.

**`kohau.sqlite.client`** — cell-wrapped SQLite client. Wraps a
single connection inside an [ahu](https://github.com/kaikailang-org/ahu)
cell (a fiber + typed mailbox + recursive step). The cell **owns
the connection lifecycle** (opens on spawn, closes on `Shutdown`)
and is the **sole owner of the `Ffi` boundary** — callers run in
`Actor[SqlMsg]`, never `Ffi`. The scope-based constructor and the
typed helpers:

- `with_client(path, body)` — open a connection, spawn the cell,
  call `body` with the client `Pid[SqlMsg]`. Scope-based because
  kaikai's region-brand walker forbids a `Pid` from escaping the
  scope that minted it (the same constraint that gives ahu's
  `with_cell` its shape).
- `exec(c, sql, binds)` — no-row statement (DDL, INSERT, DELETE,
  UPDATE). Returns `Ok(changes)` or `Err(msg)`.
- `query_row(c, sql, binds)` — ≤1-row query. Returns
  `Ok(Some(cols))`, `Ok(None)`, or `Err(msg)`.
- `query_rows(c, sql, binds)` — multi-row query. Returns
  `Ok([[String]])` (every row's columns, possibly empty) or
  `Err(e)`. Column count is discovered from the statement, so
  `SELECT *` works without declaring a width.
- `query_scalar(c, sql, binds)` — single-Int-column query
  (COUNT, MAX). Returns `Ok(n)` or `Err(e)`.
- `with_tx(c, body)` — transaction scope. `BEGIN` on entry,
  `COMMIT` if `body` returns `Ok`, `ROLLBACK` if it returns `Err`.
  The body's `Result` is threaded out; atomicity is all-or-nothing.
  `with_tx_mode` picks the locking mode.
- `fold_chunks(c, sql, binds, size, init, step)` — fold a result
  set a chunk at a time, never holding more than one chunk. For
  scans too large to materialise; `open_cursor` / `fetch_chunk` /
  `close_cursor` sit underneath it.
- `cache_stats(c)` — compiled-statement count and cache occupancy.
- `close(c)` — tell the cell to close the connection and exit.

Values are bound positionally via `[Bind]` — text, integer, real,
blob and NULL — and the client never concatenates a value into SQL.
Failure is `SqliteError`, which keeps libsqlite3's result code apart
from the message. The protocol is high-level on purpose: prepare /
step / finalize stay inside the cell and never cross the mailbox,
and a cursor is addressed by id for the same reason. Spec:
`docs/design.md`. End-to-end smoke: `tests/client_roundtrip.kai`
and `tests/client_errors.kai`.

**`kohau.postgres`** — low-level PostgreSQL client. Mirrors
`kohau.sqlite`'s shape (typed `Conn` / `Res` handles,
`Option`-returning constructors) over libpq, bridged by
`c/postgres_shim.{c,h}`. Supported range: libpq 12+ against server
12+.

Values are **always** bound as parameters, never spliced into SQL.
The FFI cannot pass `PQexecParams`'s `paramValues` string array, so
the shim accumulates binds per connection and hands the vector over
at exec time. The module deliberately exposes no escaping helper:
offering one invites building statements by concatenation, and no
path in the surface needs it.

Result diagnostics carry `result_sqlstate` — the five-character
SQLSTATE, stable across server versions and locales, unlike the
message text.

**`kohau.postgres.client`** — cell-wrapped PostgreSQL client. Same
shape as the SQLite client: an ahu cell owns the connection,
callers send `exec` / `query_row` / `query_rows` over a typed
mailbox and run in `Actor[PgMsg]` with no `Ffi` in their row.
`with_tx` brackets a body in BEGIN/COMMIT/ROLLBACK.

Differences from the SQLite client, all forced by the database
rather than chosen:

- **No `query_scalar`** — libpq returns every value as text, so a
  scalar query is `query_row` plus a caller-side parse.
- **Errors carry SQLSTATE** separately from the message.
- **`query_row` fails** on a statement returning more than one row
  instead of truncating.
- **A failed statement poisons its transaction** until it ends
  (25P02), so a body that swallows an inner `Err` fails at COMMIT
  rather than committing partial work.
- **Binds are `PgBind`** (`PgText` / `PgNull`), not `Bind` — the
  low-level surface already exports a `Bind`, and a consumer
  importing both would otherwise have two in scope.

**Opt-in reconnection** — `client.with_reconnecting_client`. When a
statement fails because the connection dropped, the cell opens a
fresh one and carries on. Queries are re-run on it; **writes are
not**. A statement that failed on a dropped connection has an
unknown outcome (the server may have applied it and died before
acknowledging), so replaying an INSERT could duplicate it; a write
reports `08006` and leaves the retry decision to the caller. An
open transaction does not survive a reconnect either, so
transaction-scoped work always fails as a whole.

## Building

kohau's modules bind libsqlite3 and libpq through C shims
(`c/sqlite_shim.{c,h}`, `c/postgres_shim.{c,h}`) and, since the
cell-wrapped client, depend on the `ahu` package. `kai build` is the
driver: it resolves the `ahu` dependency (`kai install` populates
the cache and writes `kai.lock`), and the shim sources + link flags
are passed through `CFLAGS`, which the `kai` wrapper forwards to its
underlying `cc`. This mirrors henua's Makefile and the idiomatic
`lnds/uira` raylib pattern — no raw `kaic2` invocation is needed.

Requirements:

- `kai` on `PATH`, version 0.91.0+ (git-dep resolution needs
  0.83.0+; the FFI v2 fixed-width boundary annotations the extern
  declarations use need 0.91.0+). Verified against 0.124.1.
- libsqlite3 headers + library.
- libpq 12+ headers + library, for the Postgres targets only.

Library paths are **discovered**, not assumed: `SQLITE_INC` /
`PG_INC` and their `_LIB` counterparts come from `pkg-config` and
`pg_config`, falling back to Homebrew's kegs. They are `?=`, so
`make SQLITE_INC=... PG_LIB=...` still wins if an install lives
somewhere else.

Run the fixtures:

```sh
make tier0    # compile every fixture (runs kai install if needed)
make tier1    # compile + run + diff against goldens
```

The Postgres fixtures need a running server, so they sit on their
own targets and stay out of the default `tier1`. Point them at a
server through libpq's environment (`PGHOST`, `PGDATABASE`, …):

```sh
make tier0-pg            # compile the pg fixtures
make tier1-pg            # compile + run + diff (5 fixtures)
make tier1-pg-reconnect  # stops/starts the server mid-run; needs PGDATA_DIR
```

## Foundational principle: kohau builds on ahu

**kohau is built on top of [ahu](https://github.com/kaikailang-org/ahu),
not on raw kaikai primitives.** Database connections, prepared
statement caches, and connection pools are long-running stateful
entities — exactly what ahu cells (Layer 2) and restart helpers
(Layer 3) exist for. The raw FFI to libsqlite3 or libpq (and, later,
the native wire-protocol machinery for Postgres) is the *low-level*
surface; the *ergonomic* surface that downstream code uses is the
cell-wrapped client, which gives request/reply query execution,
supervised lifecycle, and pipe composition.

Concretely: every backend driver exposes two shapes — a low-level
function form (`open`, `execute`, `prepare`, `close` operating
directly on the FFI/wire handle) and a cell-based wrapper form
(`with_client(config, body)`) that runs the client inside an ahu
cell with proper connection lifecycle and, as they land, statement
caching and restart-on-failure semantics. Postgres has the first
piece of the latter today in `with_reconnecting_client`.

This is not optional. Implementations that bypass ahu — connection
state stored in globals, raw `spawn` for background work, ad-hoc
reconnect loops — are out of scope for kohau. If a use case can't
be expressed via ahu primitives, the gap gets filed against ahu,
not worked around inside kohau.

Implementer agents working on kohau MUST read ahu's `docs/design.md`
before writing module surfaces, and prefer ahu primitives over raw
kaikai primitives wherever both are available.

## Why

kohau is the *inscribed tablet* layer — the substrate on which data
is written. The name is Rapa Nui (the wooden tablet that carried the
rongorongo script). It is **infrastructure**, not domain. kohau
provides raw DB clients (connection, query, type codecs, transaction
control). DDD vocabulary — Aggregate, Repository, EventBus — lives in
henua on top of kohau.

The split keeps each layer focused:

- kohau may be used **without DDD** (CLI scripts that only run
  queries, ETL pipelines, simple tools).
- DDD adapters live in **henua**, not kohau (so
  `ConnectionConfig` / `SslMode` / `RowDescription` never leak into
  the domain model).
- Each backend driver evolves independently inside kohau.

Mirrors Elixir Postgrex+Ecto, Go database/sql+gorm, Rust
tokio-postgres+diesel.

## v0.1 — SQLite first (shipped)

The v0.1 goal was one driver working end-to-end — a kaikai program →
kohau → libsqlite3 → file — so that henua's `SqliteRepository[A, I]`
could be a thin adapter. That path is closed and covered by tier1.

Everything this section originally scoped is in, and the driver
picked up more along the way:

- **Every storage class.** `Bind` covers text, integer, real, blob
  and NULL. BLOBs cross the FFI boundary hex-encoded — kaikai's
  `String` reaches C NUL-terminated, so raw bytes would truncate at
  the first zero.
- **WAL by default.** Connections open in WAL mode, so one writer
  and many readers proceed concurrently. Advisory: a database that
  cannot take it (`:memory:`, a filesystem without shared memory)
  keeps its mode and works anyway.
- **A prepared-statement cache.** Compiled statements are reused
  across operations, keyed by SQL text — twenty identical inserts
  compile one statement. `cache_stats` exposes the compile count, so
  the optimisation is checkable rather than assumed.
- **Transaction modes.** `with_tx` is the deferred default;
  `with_tx_mode` issues `BEGIN IMMEDIATE` / `EXCLUSIVE` for the
  read-then-write that would otherwise fail at the write.
- **Cursors for large scans.** `fold_chunks` walks a result set a
  bounded chunk at a time instead of materialising it, with
  `open_cursor` / `fetch_chunk` / `close_cursor` underneath.
- **Typed errors.** `SqliteError` keeps the result code (and the
  extended code that says *which* constraint broke) apart from the
  message, so callers branch on the code rather than on wording that
  changes between releases.

Beyond the original scope, v0.1 also gained typed `Db` / `Stmt`
handles (v0.3.0) and the cell-wrapped client that seals `Ffi` from
callers.

What is still open:

- **Rows arrive as text.** Every column is read through
  `column_text`, so a BLOB needs `hex()` around it to come back
  byte-exact. A typed row protocol would change the reply shape.
- **No connection pool**, and no restart-on-failure — the Postgres
  client has the first version of the latter.

## Postgres — libpq today, native wire protocol still the goal

**What ships now** is a client over **libpq via FFI** (see *What
ships* above): the fastest route to a working Postgres path, and
enough to validate the surface — parameter binding, SQLSTATE-typed
failure, transaction scoping and reconnection are all exercised
end-to-end against a real server.

**The 2026-04-28 decision still stands**: the native wire protocol
in pure kaikai (Simple + Extended Query, SCRAM-SHA-256 auth, type
codecs, basic connection pool; TLS via FFI to OpenSSL/LibreSSL) is a
language-credibility statement, mirroring Postgrex / pgx /
tokio-postgres. Estimated 3-6 weeks of focused work. libpq is the
intermediate step, not the replacement.

Going through libpq first buys a stable reference: the cell-wrapped
surface, the fixtures and the goldens are all backend-agnostic, so
the native driver can be brought up behind the same API and
differentially tested against the libpq one rather than against a
specification alone.

Eventually (post-Postgres): MySQL, ClickHouse, DuckDB.

## Layout

```
kohau/
├── README.md
├── CHANGELOG.md
├── kai.toml
├── Makefile                    # tier0/tier1 targets, library discovery
├── docs/
│   ├── design.md               # the cell-wrapped client protocol
│   └── known-regressions.md    # upstream blockers the surface works around
├── kohau/                      # the importable kaikai modules
│   ├── sqlite.kai              # low-level: handles, bind/step/column, tx
│   ├── sqlite/client.kai       # cell-wrapped: exec / query_* / with_tx
│   ├── postgres.kai            # low-level over libpq, SQLSTATE diagnostics
│   └── postgres/client.kai     # cell-wrapped + opt-in reconnection
├── c/                          # shims for shapes kaikai's FFI cannot express
│   ├── sqlite_shim.{c,h}
│   └── postgres_shim.{c,h}
└── tests/                      # tier1 fixtures + goldens (.out.expected)
```

Each driver keeps the same two-layer split: a `<driver>.kai` with
the low-level FFI surface, and a `<driver>/client.kai` with the
cell-wrapped one. There is no shared `DbClient` protocol type yet —
the two clients converged on the same *shape* by construction, but
the differences libpq forces (see above) mean a common signature
would have to paper over them. It gets extracted when a third driver
shows which parts are genuinely common.

## License

TBD. Will match the kaikai ecosystem license.
