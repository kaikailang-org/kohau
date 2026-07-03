# kohau

DB clients for [kaikai](https://github.com/kaikailang-org/kaikai). The
persistence substrate that sits between the language's effects and
the DDD vocabulary of [henua](https://github.com/kaikailang-org/henua).

> **Status:** SQLite shipped in two layers — the low-level
> surface (`kohau.sqlite`, typed FFI handles) and the cell-wrapped
> ergonomic client (`kohau.sqlite.client`, an ahu cell that owns
> the connection lifecycle and seals `Ffi` from callers). The
> client gives request/reply query execution with the FFI
> confined to one fiber. Connection pool, restart-on-failure, a
> statement cache, and a multi-row query protocol are follow-ups.
> Postgres is later.

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
  `Err(msg)`. Column count is discovered from the statement, so
  `SELECT *` works without declaring a width.
- `query_scalar(c, sql, binds)` — single-Int-column query
  (COUNT, MAX). Returns `Ok(n)` or `Err(msg)`.
- `with_tx(c, body)` — transaction scope. `BEGIN` on entry,
  `COMMIT` if `body` returns `Ok`, `ROLLBACK` if it returns `Err`.
  The body's `Result` is threaded out; atomicity is all-or-nothing.
- `close(c)` — tell the cell to close the connection and exit.

Values are bound positionally via `[Bind]` (`BindText` / `BindInt`
in v1) — the client never concatenates a value into SQL. The
protocol is high-level on purpose: prepare / step / finalize stay
inside the cell and never cross the mailbox. Spec:
`docs/design.md`. End-to-end smoke: `tests/client_roundtrip.kai`
and `tests/client_errors.kai`.

## Building

kohau's modules bind libsqlite3 through a C shim (`c/sqlite_shim.{c,h}`)
and, since the cell-wrapped client, depend on the `ahu` package.
`kai build` is the driver: it resolves the `ahu` dependency
(`kai install` populates the cache and writes `kai.lock`), and the
shim sources + `-lsqlite3` are passed through `CFLAGS`, which the
`kai` wrapper forwards to its underlying `cc`. This mirrors henua's
Makefile and the idiomatic `lnds/uira` raylib pattern — no raw
`kaic2` invocation is needed.

Requirements:

- `kai` on `PATH`, version 0.91.0+ (git-dep resolution needs
  0.83.0+; the FFI v2 fixed-width boundary annotations the extern
  declarations use need 0.91.0+). Verified against 0.98.0.
- libsqlite3 headers + library. macOS Homebrew:
  `/opt/homebrew/opt/sqlite/{include,lib}`. Override via
  `make SQLITE_INC=... SQLITE_LIB=...` if the install lives
  elsewhere.

Run the fixtures:

```sh
make tier0    # compile every fixture (runs kai install if needed)
make tier1    # compile + run + diff against goldens
```

## Foundational principle: kohau builds on ahu

**kohau is built on top of [ahu](https://github.com/kaikailang-org/ahu),
not on raw kaikai primitives.** Database connections, prepared
statement caches, and connection pools are long-running stateful
entities — exactly what ahu cells (Layer 2) and restart helpers
(Layer 3) exist for. The raw FFI to libsqlite3 (or the wire-protocol
machinery for Postgres) is the *low-level* surface; the *ergonomic*
surface that downstream code uses is the cell-wrapped client, which
gives request/reply query execution, supervised lifecycle, and pipe
composition.

Concretely: every backend driver exposes two shapes — a low-level
function form (`open`, `execute`, `prepare`, `close` operating
directly on the FFI/wire handle) and a cell-based wrapper form
(`with_client(config, body)`) that runs the client inside an ahu
cell with proper connection lifecycle, statement caching, and
restart-on-failure semantics.

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

## v0.1 — SQLite first

The v0.1 goal is one driver that works end-to-end:

- **`SqliteClient`** — FFI to system `libsqlite3`. Bindings cover
  `sqlite3_open` / `prepare_v2` / `step` / `column_*` / `bind_*` /
  `finalize` / `close` / `errmsg`. ~30-50 FFI declarations.
- Type codecs: INTEGER / REAL / TEXT / BLOB / NULL.
- WAL mode default for concurrency.
- Statement caching via fiber-local handle map.
- Transactions (`BEGIN` / `COMMIT` / `ROLLBACK`) with deferred /
  immediate / exclusive modes.
- Result iteration via a streaming row interface.
- Errors typed as `SqliteError` with the canonical libsqlite3 error
  codes preserved.

This is enough to validate the full stack: a kaikai program → kohau
→ libsqlite3 → file. From here, henua's `SqliteRepository[A, I]`
becomes a thin adapter.

## Post-v0.1 — Postgres native

Native wire protocol in pure kaikai (Simple + Extended Query,
SCRAM-SHA-256 auth, type codecs, connection pool basic). TLS via FFI
to OpenSSL/LibreSSL. Decision pinned 2026-04-28: implementing the
wire protocol natively is a language-credibility statement (mirrors
Postgrex / pgx / tokio-postgres). Estimated 3-6 weeks of focused
work, deferred until SQLite ships and henua's `SqliteRepository`
validates the surface end-to-end.

Eventually (post-Postgres): MySQL, ClickHouse, DuckDB.

## Layout (target)

```
kohau/
├── README.md
├── kai.toml
├── docs/
│   ├── design.md
│   └── sqlite.md
├── kohau/                      # the importable kaikai modules
│   ├── sqlite.kai              # SqliteClient + connection / query / tx
│   ├── sqlite_ffi.kai          # FFI declarations against libsqlite3
│   ├── sqlite_types.kai        # type codecs INTEGER / REAL / TEXT / BLOB / NULL
│   ├── errors.kai              # SqliteError + shared error patterns
│   └── client.kai              # DbClient protocol (shared shape for future drivers)
├── examples/
│   ├── hello_sqlite/           # open, create table, insert, select
│   └── tx_demo/                # transactions BEGIN/COMMIT/ROLLBACK
└── tests/
    └── ...                     # tier1 fixtures
```

## License

TBD. Will match the kaikai ecosystem license.
