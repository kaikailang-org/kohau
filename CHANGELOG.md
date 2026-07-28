# Changelog

All notable changes to kohau are tracked in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/) once
1.0.0 ships.

## [Unreleased]

### Added

- **`kohau.postgres` — low-level PostgreSQL client.** Mirrors
  `kohau.sqlite`'s shape (typed `Conn` / `Res` handles,
  `Option`-returning constructors) over libpq, bridged by
  `c/postgres_shim.{c,h}`.

  Values are **always** bound as parameters, never spliced into SQL.
  The FFI cannot pass `PQexecParams`'s `paramValues` string array, so
  the shim accumulates binds per connection and hands the vector to
  `PQexecParams` at exec time. The module deliberately exposes no
  escaping helper: offering one invites building statements by
  concatenation, and the surface has no path that would need it.

  Supported range: **libpq 12+ against server 12+**. The shim calls
  only protocol-v3 entry points (stable since 7.4), but 12 is the
  floor that is realistically testable.

  Result diagnostics carry `result_sqlstate` — the five-character
  SQLSTATE, stable across server versions and locales, unlike the
  message text.

- **`kohau.postgres.client` — cell-wrapped PostgreSQL client.** The
  postgres counterpart of `kohau.sqlite.client`, and the ergonomic
  surface: an ahu cell owns the connection, callers send high-level
  operations (`exec` / `query_row` / `query_rows`) over a typed
  mailbox and run in `Actor[PgMsg]` with **no `Ffi`** in their row.
  `with_tx` brackets a body in BEGIN/COMMIT/ROLLBACK.

  Differences from the SQLite client, all forced by the database
  rather than chosen: no `query_scalar` (libpq returns every value as
  text, so a scalar query is `query_row` plus a caller-side parse);
  errors carry SQLSTATE separately from the message; a statement
  returning more than one row fails `query_row` instead of
  truncating; and a failed statement inside a transaction poisons it
  until the transaction ends (25P02), so a body that swallows an
  inner `Err` fails at COMMIT rather than committing partial work.

  The bind type is `PgBind` (`PgText` / `PgNull`), not `Bind` —
  `kohau.postgres` already exports a `Bind` for the low-level
  surface, and a consumer importing both would otherwise have two in
  scope.

- **Opt-in reconnection: `client.with_reconnecting_client`.** When a
  statement fails because the connection dropped, the cell opens a
  fresh one and carries on. Queries are re-run on it; **writes are
  not** — a statement that failed on a dropped connection has an
  unknown outcome (the server may have applied it and died before
  acknowledging), so replaying an INSERT could duplicate it. A write
  reports `08006` and leaves the retry decision to the caller. An
  open transaction does not survive a reconnect either, so
  transaction-scoped work always fails as a whole.

  Detection keys on the SQLSTATE being *empty*, not on `PQstatus`:
  libpq updates the status lazily, so the statement that discovers
  the death still reads `CONNECTION_OK` and only the following call
  reports `CONNECTION_BAD` — a reconnect keyed on the status misses
  the very failure that should trigger it. A server-rejected
  statement always carries a five-character code; one that died with
  the connection carries none.

- **`client.commit`, and `postgres.cmd_status` /
  `transaction_status`.** PostgreSQL answers a COMMIT on an aborted
  transaction with a *successful* result whose command tag reads
  `ROLLBACK`. `with_tx` reported that as `Ok`, claiming work had
  landed when nothing had; it now routes commits through
  `client.commit`, which inspects the tag and returns `Err`. The tag
  is the only signal — status, SQLSTATE and message all describe a
  clean success.

- **`tier0-pg` / `tier1-pg` Makefile targets** covering five
  fixtures: parameter binding (including that injection payloads
  round-trip as inert data), the low-level surface end to end (NULL
  vs empty string, `cmd_tuples`, column names), failure paths by
  SQLSTATE, the cell-wrapped client, and transaction scoping. Kept
  out of the default `tier1` because they need a running server;
  point them at one via libpq's environment.

- **`tier1-pg-reconnect`**, a sixth fixture on its own target: it
  stops and starts the server mid-run through
  `tests/pg-restart-driver.sh`, so the reconnect path is exercised
  against a connection that really dropped. Needs `PGDATA_DIR` as
  well as the connection settings, since it drives `pg_ctl`.

### Changed

- **The Makefile discovers library paths instead of assuming them.**
  `SQLITE_INC` / `PG_INC` and their `_LIB` counterparts now come from
  `pkg-config` and `pg_config`, falling back to Homebrew's kegs, and
  are `?=` so an explicit override still wins. macOS keeps both
  libraries keg-only where the compiler does not look, while Linux
  puts them on the default search path; the empty case emits no
  `-I`/`-L` rather than a bogus directory. Prerequisite for building
  on a second platform.

### Fixed

- **`kai.toml` declares `edition = "hanga-roa"`.** 0.4.0 declared
  `orongo`, which is not a released edition — hanga-roa is the
  current one.

- **The postgres shim asserts its handle-width assumption.** Handles
  cross the FFI boundary as `int64_t`; a platform where a pointer
  does not fit now fails to compile rather than silently truncating
  one.

## [0.4.0] — 2026-07-23

### Changed

- **The sources move to the orongo edition (kaikai 0.104).** kaikai
  0.101 flipped `Result`'s type-argument order from
  `Result[Error, Ok]` to `Result[Ok, Error]`, so every annotation in
  the package stopped typing against the current compiler. The 18
  affected annotations were rewritten with `kai migrate` and
  `kai.toml` now declares `edition = "orongo"`.

  `Ok` and `Err` are position-independent, so consumers that match by
  constructor are unaffected. Consumers that *annotate* a kohau
  result — `Result[String, Int]` for `exec`, and the same shape for
  `query_scalar` and the client surface — must flip their own
  annotations to `Result[Int, String]` and declare the orongo
  edition.

  The rewrite runs through the fmt-writer, so the sources also picked
  up canonical 0.104 formatting. That is the bulk of the diff; the
  semantic change is the argument order alone.

- **`ahu` moves to v0.2.1** from v0.1.0. The release restarts
  `Transient` bodies on a runtime fault, not only on an escalated
  cancel, and installs effect handlers per fiber — both relevant to a
  connection owned by a cell step function. kohau's own surface did
  not change.

## [0.3.0] — 2026-07-03

### Changed

- **Typed handles on the low-level surface — `kohau.sqlite` now
  speaks `Db` / `Stmt`, not `Int`.** Adjusting kohau to the
  kaikai 0.98 toolchain (FFI v2 shipped in 0.91.0; kohau's surface
  predated it, verified at 0.86.1):

  - `open(path) : Option[Db]` and `prepare(db, sql) : Option[Stmt]`
    replace the `Int`-returning forms with their `0`-means-failure
    sentinel. Failure is a typed absence; a `Db` cannot be passed
    where a `Stmt` is expected (or vice versa), and every other
    operation takes the typed handle.
  - The intended FFI v2 form — `extern "C" opaque Db / Stmt` — is
    **blocked upstream**: an opaque extern type lowers to a private
    nominal type and cannot appear in `pub` signatures (documented
    with repro in `docs/known-regressions.md`). The handles are
    nominal wrapper records over the shim's pointer-as-`Int`
    representation instead; when opaque types become exportable the
    swap is internal to `kohau/sqlite.kai` and the public surface
    does not change.
  - The extern declarations adopt FFI v2 **fixed-width boundary
    annotations**: `I32` where the shim's C type is `int` (result
    codes, bind/column indexes, `changes`, `column_count`). This
    closes a latent width mismatch — the old declarations bound C
    `int` returns as kaikai `Int` (`int64_t`), which read garbage
    high bits on ABIs that do not zero-extend. The shim's handle
    and 64-bit-value types move from `long` to `int64_t` for the
    same exactness.
  - `kohau.sqlite.client`'s public surface is unchanged (it deals
    in `Pid[SqlMsg]`). Internally the cell state becomes
    `Option[Db]`, which also delivers the **typed open-failure
    path** that was previously a follow-up: when `open` fails the
    cell replies `SqlErr("no connection: open failed")` to every
    request instead of running SQLite ops against a `0` handle.
    New fixture `tests/client_open_failure.kai` drives it (every
    helper errs typed, `close` still shuts the cell down cleanly).

  BREAKING for direct users of the low-level surface (all decls
  are `#[unstable]`; henua consumes the client surface, which did
  not move). tier1 grows from 5 to 6 fixtures, green under kaikai
  0.98.0 on both backends (`native` and `--backend=c`
  parity-checked for the low-level fixture).

- **Toolchain baseline: kaikai 0.98.0.** tier1 re-verified; of the
  two upstream regressions documented at 0.86.1, the transitive
  `#[unstable]` warnings are resolved and the `awk` stderr noise
  still reproduces (`docs/known-regressions.md` re-verification
  notes). Minimum `kai` moves to 0.91.0+ (FFI v2 fixed-width
  annotations).

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
  `[[String]]`) — *delivered in [0.3.0]*; a streaming /
  chunked reply (or an ahu `Stream` source) for unbounded scans
  remains a follow-up.
- **transaction scope.** `with_tx(c, body)` — *delivered in
  [0.3.0]*; nested transactions (savepoints) and
  IMMEDIATE/EXCLUSIVE modes remain follow-ups.
- **richer binds + columns.** `Real` / `Blob` / `Null` binds and
  typed column reads beyond TEXT/Int.
