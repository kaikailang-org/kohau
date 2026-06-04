# Changelog

All notable changes to kohau are tracked in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/) once
1.0.0 ships.

## [Unreleased]

### Added

- **`kohau.sqlite.client.query_rows(c, sql, binds)` — multi-row
  query.** Returns every result row's TEXT columns as `[[String]]`
  in result-set order (empty list for no rows), or `Err(msg)` on a
  SQLite failure. This unblocks `henua.repository.all` over a
  persistent store — the consumer that motivated it.

  The column count is discovered at runtime from the prepared
  statement via a new low-level primitive **`kohau.sqlite.column_count`**
  (and its shim function `kai_sqlite_column_count` wrapping
  `sqlite3_column_count`), so `SELECT *` and explicit column lists
  both work without the caller declaring a width. This was chosen
  over a caller-supplied `ncols` parameter: a hand-counted width
  that drifts from the SELECT is a silent bug, whereas
  `column_count` is a primitive the low-level surface should expose
  regardless.

  v1 **materialises** the whole result set before replying — the
  reply is a single `[[String]]`, not a stream. Right for the
  bounded result sets the layers above kohau need today; a
  streaming / chunked protocol (or an ahu `Stream` source) for
  unbounded scans is a follow-up. New fixture
  `tests/client_query_rows.kai` covers empty / single-column /
  multi-column / WHERE-filtered-with-bind. tier1 grows from 4 to 5
  fixtures, green under kaikai 0.86.1. Every new `pub` is
  `#[unstable]`.

- **`kohau.sqlite.client.with_tx(c, body)` — transaction scope.**
  Brackets `body` in `BEGIN` / `COMMIT`-or-`ROLLBACK`: commits if
  the body returns `Ok`, rolls back if it returns `Err`. The body's
  `Result` is threaded out to the caller — `with_tx` does not invent
  its own success value. Honest to the language: there is no hidden
  control flow that "throws" mid-body; the failure channel is the
  value the body returns, so a body composed of `client.exec` /
  `query_*` calls (each already a `Result`) threads its failures out
  naturally. `tx` is the same pid as `c` because SQLite transactions
  are per-connection; passing it as the body argument documents
  which statements are bracketed. v1 is single-level (no nested
  `with_tx` — SQLite rejects nested `BEGIN`; savepoints are a
  follow-up) and issues a plain deferred `BEGIN` (no
  IMMEDIATE/EXCLUSIVE mode control yet). New fixture
  `tests/client_tx.kai` proves all three routes (commit persists,
  explicit rollback discards, mid-body failure rolls back the whole
  tx atomically). tier1 grows from 3 to 4 fixtures, all green under
  kaikai 0.86.1. Every `pub` stays `#[unstable]`.

## [0.2.0] — 2026-06-04

### Added

- **`kohau/sqlite/client.kai` — cell-wrapped SQLite client.**
  The ergonomic surface that wraps a single connection inside an
  [ahu](https://github.com/kaikailang-org/ahu) cell. This is the
  foundational principle the README has documented since v0.1
  finally realised in code: the connection is a long-running
  stateful entity owned by an `ahu.cell` step function, not a raw
  `Int` handle passed around by callers.

  - `with_client(path, body)` — scope-based constructor. Opens the
    connection, spawns the cell-fiber, calls `body` with the client
    `Pid[SqlMsg]`. Scope-based (not `connect(path) : Pid`) because
    kaikai's region-brand walker forbids a `Pid` from escaping the
    scope that minted it — the same constraint that gives ahu's
    `with_cell` its `with_X(initial, step, body)` shape. Verified
    empirically: a returning form is rejected with `Pid[Msg] cannot
    escape ...'s structured-concurrency scope`.
  - Typed helpers `exec` / `query_row` / `query_scalar` / `close`,
    all in the `Actor[SqlMsg]` effect — **no `Ffi` at the call
    site**. The cell-fiber is the sole owner of the FFI boundary.
    This is the load-bearing property for the layers above kohau:
    henua's `SqliteRepository` drops `/ Ffi` from its operations.
  - Protocol (`SqlMsg`, `SqlReply`, `Bind`): mono-mailbox per
    kaikai's `ask` constraint (request and reply travel the same
    mailbox), with the reply payload split into a **pid-free**
    `SqlReply` type so the per-operation helpers can *return* a
    reply value (the walker rejects returning a type that could
    embed a `Pid`). Binds are positional (`BindText` / `BindInt`).
    The protocol is high-level — prepare / step / finalize stay
    inside the cell and never cross the mailbox.
  - The cell survives a failed statement: each operation prepares
    and finalizes its own statement, so a SQL error leaves the
    connection usable for the next call (liveness, exercised by
    `tests/client_errors.kai`).
  - Every `pub` is `#[unstable]` for the Hanga Roa edition.

  New fixtures: `tests/client_roundtrip.kai` (full request/reply
  surface against `:memory:` — create / insert / count / find-hit /
  find-miss / delete) and `tests/client_errors.kai` (error
  propagation through the mailbox + post-error liveness). tier1
  grows from 1 to 3 fixtures, all green under kaikai 0.86.1.

### Changed

- **`kai.toml` gains a dependency on `ahu`** (git-dep,
  `ref = "v0.1.0"`). The cell-wrapped client is built on
  `ahu.cell`. The `[unstable]` opt-in block adds `client` and
  `cell` for the in-tree fixtures (warning-free build).

- **Makefile migrated from raw `kaic2` to the `kai build` driver.**
  The previous Makefile invoked `kaic2` directly with `--path`
  flags, which does not resolve git/path dependencies. The new
  client module imports `ahu.cell`, so dependency resolution is now
  load-bearing: the Makefile uses `kai build` (which owns stdlib
  prelude assembly, package-path resolution, and edition gates) and
  passes the shim sources + `-lsqlite3` through `CFLAGS`. A
  `kai.lock` target runs `kai install` when the lockfile is absent
  or stale. This mirrors henua's Makefile and the idiomatic
  `lnds/uira` pattern.

### Follow-ups (not in this release)

- **restart-on-failure.** *Resolved as a not-goal for SQLite* after
  design review (see `docs/design.md` §*Why restart-on-failure is a
  not-goal for SQLite*): for an embedded DB, transient failures are
  already handled per-operation, the only failure a restart could
  catch is a deterministic FFI panic (which must not be retried),
  and restart mid-`with_tx` would silently break atomicity. There is
  also an upstream blocker (ahu's `restartable_cell` leaks the step's
  `Ffi` into the driver — `docs/known-regressions.md`). Restart
  becomes real with the Postgres driver, where a network connection
  genuinely drops and the cell's state is the reconnection recipe,
  not a live handle.
- **connection pool.** Multiple supervised client cells behind a
  router. A pool only earns its keep once supervision does — i.e.
  with Postgres, not SQLite.
- **statement cache.** An LRU `Map[String, Int]` in the cell state.
  Deferred — adds invalidation complexity over raw FFI pointers with
  no fixture demonstrating the cost matters.
- **multi-row query protocol.** `query_rows` (materialised
  `[[String]]`) — *delivered in [Unreleased]*; a streaming /
  chunked reply (or an ahu `Stream` source) for unbounded scans
  remains a follow-up.
- **transaction scope.** `with_tx(c, body)` — *delivered in
  [Unreleased]*; nested transactions (savepoints) and
  IMMEDIATE/EXCLUSIVE modes remain follow-ups.
- **richer binds + columns.** `Real` / `Blob` / `Null` binds and
  typed column reads beyond TEXT/Int.
