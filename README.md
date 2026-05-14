# kohau

DB clients for [kaikai](https://github.com/kaikailang-org/kaikai). The
persistence substrate that sits between the language's effects and
the DDD vocabulary of [henua](https://github.com/kaikailang-org/henua).

> **Status:** scaffolding. v0.1 target: SQLite first. Postgres later.

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
