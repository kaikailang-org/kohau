# kohau design

kohau is the persistence substrate of the kaikai ecosystem stack
(`kaikai → ahu → kohau → henua → manutara/hopu`). It provides DB
clients; the DDD vocabulary that consumes them lives one layer up in
henua. This document pins the design of the two surfaces kohau ships
for SQLite and the decisions behind the cell-wrapped client.

## Two surfaces, one driver

Every backend driver in kohau exposes two shapes:

1. **Low-level surface** (`kohau.sqlite`) — a one-to-one mapping to
   the backend's native API (libsqlite3 here), with raw `Int`
   handles the caller opens, prepares, steps, finalizes, and closes
   by hand. Every operation carries `Ffi`. This surface is for
   scripts, fixtures, and one-shot tooling that does not need
   connection ownership.

2. **Cell-wrapped surface** (`kohau.sqlite.client`) — the ergonomic
   form that downstream layers use. A single connection lives inside
   an `ahu` cell; callers talk to it via request/reply over a typed
   mailbox and never touch a raw handle.

This split mirrors the lineage: Elixir's Postgrex (low-level) +
Ecto (high-level), Go's `database/sql` + GORM, Rust's
tokio-postgres + diesel. The low-level surface is honest about the
backend; the cell surface is honest about lifecycles and effects.

## The cell-wrapped client

### Why a cell

A database connection is a long-running stateful entity: it has a
handle that must be opened once and closed once, it can cache
prepared statements, and it can fail and need re-establishing.
That is exactly the shape an `ahu` cell models (a fiber + typed
mailbox + recursive step function over an immutable state). The
README's foundational principle — *kohau builds on ahu, not on raw
kaikai primitives* — is realised here: the connection's `Int`
handle is the cell's state, and every operation is a message.

### Why `with_client(path, body)` and not `connect(path) : Pid`

The shape is **forced by the type system**, not chosen for style.
kaikai's region-brand walker forbids a `Pid[Msg]` from escaping the
function scope that created it — a `Pid` is branded to the actor
scope that minted it, and returning one would let it outlive that
scope. A `connect(path) : Pid[SqlMsg]` form is rejected at compile
time:

```
error: Pid[Msg] cannot escape `connect`'s structured-concurrency scope
  = note: a `Pid[Msg]` handle is bound to the actor scope that created it
  = help: process the pid inside the scope, or pass a thunk in and a value out
```

So the connection's lifetime is a lexical scope. `with_client`
opens the connection, spawns the cell, and calls `body` with the
client pid; when `body` returns, the scope ends. This is the same
constraint that gives ahu's `with_cell` its `with_X(initial, step,
body)` shape — kohau inherits it.

### Why the FFI is sealed (and how)

The load-bearing property: callers run in `Actor[SqlMsg]`, **not
`Ffi`**. The cell-fiber is the sole code that touches libsqlite3.

This does not happen automatically. `with_cell` (ahu's
constructor) shares a single open row variable between its `step`
and `body`, so wrapping a DB client with `with_cell` naively would
leak the step's `Ffi` into the body — the opposite of the goal.
Verified empirically: `with_cell(db, sql_step, driver)` forces
`driver` to carry `Ffi` to match `sql_step`.

The fix is to **not** use `with_cell`. `with_client` hand-spawns
the cell loop with `spawn_actor` and a recursive `sql_loop`:

```kai
pub fn with_client[R, e](
  path: String,
  body: (Pid[SqlMsg]) -> R / Actor[SqlMsg] + e
) : R / Spawn + Ffi + Actor[SqlMsg] + e = {
  let db  = sqlite.open(path)
  let pid = spawn_actor(() => sql_loop(db))   # Ffi lands HERE
  body(pid)                                    # body sees only Actor[SqlMsg]
}
```

`sqlite.open` and the spawned `sql_loop` (which runs the
FFI-touching step) put `Ffi` in `with_client`'s **own** row. `body`'s
row is `Actor[SqlMsg] + e` — no `Ffi`. The cell is the membrane.

### The protocol: mono-mailbox, split reply

kaikai's `Actor.send : Pid[T] -> T` ties the reply target's type to
the active handler's `Msg`, so request and reply must travel the
same mailbox (the constraint ahu documents under `cell.ask`). One
union, `SqlMsg`, carries the request variants (`Exec`, `QueryRow`,
`QueryScalar`, `Shutdown`) plus a single `Reply(SqlReply)` wrapper.

The reply payload is its **own pid-free type**, `SqlReply`
(`ExecOk` / `RowFound` / `RowAbsent` / `ScalarInt` / `SqlErr`).
This is not cosmetic: the per-operation helpers (`run_exec` etc.)
*return* a reply value to the step function, and the region-brand
walker rejects returning a type that *could* embed a `Pid` — even
when the actual variant never holds one. Splitting the reply into a
type that structurally cannot carry a pid sidesteps the rejection,
and is cleaner besides: a request needs a reply target, a reply
never does.

### Why the protocol is high-level

The mailbox is a process boundary. Exposing `prepare` / `step` /
`finalize` *across* it would send a statement handle — a raw FFI
pointer living in the cell's address space — back to the caller in
a reply: meaningless to the caller and a lifetime foot-gun. So the
cell executes **complete, atomic operations** and returns
**values**:

- `Exec` — a no-row statement (DDL, INSERT, DELETE, UPDATE,
  BEGIN/COMMIT). Reply carries `sqlite.changes()`.
- `QueryRow` — a ≤1-row statement. Reply is the row's TEXT columns
  or "absent". This is the `find`-by-primary-key shape.
- `QueryRows` — a multi-row statement. Reply is every row's TEXT
  columns as `[[String]]` (empty list for no rows).
- `QueryScalar` — a single-Int-column statement (COUNT, MAX).

`QueryRows` discovers its column count at runtime via
`sqlite.column_count` (a primitive added to the low-level surface
for exactly this), so it handles `SELECT *` and explicit column
lists without the caller declaring a width. It **materialises** the
full result set before replying: the reply is one `[[String]]`, not
a stream. This is right for the bounded result sets the consumer
above kohau needs (henua's `repository.all` over a per-aggregate
table). A streaming / chunked reply over the mailbox — with the
back-pressure design that entails — is a follow-up for unbounded
scans; no fixture motivates it yet.

### Liveness under failure

Each operation prepares and finalizes its own statement. A SQL
error in one operation therefore leaves the connection usable for
the next — a failed statement does not poison the cell. This is the
liveness property that makes the cell safe to keep open across many
operations; `tests/client_errors.kai` exercises it (three failing
statements followed by a succeeding one).

## Not-goals (v1)

- **No statement cache.** An LRU `Map[String, Int]` in the cell
  state is the obvious optimisation, deferred until a fixture shows
  the prepare/finalize-per-call cost matters.
- **No restart-on-failure yet.** Wrapping the cell in ahu's
  `restartable_cell` needs a reconnection design (re-`open` resets
  the handle; in-flight statements and open transactions are lost).
- **No connection pool.** Depends on the restart story.
- **No nested transactions / isolation modes.** `with_tx` ships
  (see below) but is single-level and deferred-`BEGIN` only;
  savepoint nesting and IMMEDIATE/EXCLUSIVE modes are follow-ups.

## Transactions (`with_tx`)

`with_tx(c, body)` brackets `body` in `BEGIN` / `COMMIT`-or-
`ROLLBACK`. The design choices, all forced by or aligned with the
language:

- **The body returns a `Result`; the arm decides commit vs
  rollback.** kaikai has no exceptions, so the failure channel for a
  transactional body is its return value: `Err` rolls back, `Ok`
  commits. There is no hidden control flow — a body that swallows a
  failed inner op and returns `Ok` *will* commit, by the caller's
  explicit choice. This is honest in a way an exception-driven
  "rollback on throw" is not.
- **`with_tx` threads the body's `Result` out** rather than
  inventing its own success value. The commit/rollback is a side
  effect of which arm the body returned; the caller gets back
  exactly what the body produced (or a `BEGIN`/`COMMIT`-failure
  `Err`).
- **`tx` is the same pid as `c`.** SQLite transactions are
  per-connection, and the cell owns one connection, so the
  "transaction handle" is the client pid itself. It is passed as the
  body argument purely to make the scope read clearly and document
  which statements are bracketed.
- **Rollback errors do not mask the cause.** If the body returns
  `Err` and the `ROLLBACK` then fails, the body's original `Err`
  propagates — the rollback failure is swallowed because the
  original cause is the more useful signal.

`with_tx` is built on `exec` (BEGIN/COMMIT/ROLLBACK are no-row
statements), so it inherits the FFI-sealing and `Actor[SqlMsg]`
properties of the rest of the client — no new effect surface.

## External dependencies on kaikai

- **The region-brand walker** shapes the entire public surface
  (`with_client` scope form, `SqlReply` pid-free split). These are
  not worked around; they are the contract. See
  `docs/known-regressions.md` for toolchain issues observed against
  the current kaikai release.
