# Known regressions

Open issues *outside this repository* that block or constrain kohau
against the current kaikai release. Each entry names the upstream
artefact, the observed symptom, the workaround in kohau (if any),
and whether it blocks tier1.

This file follows ahu's convention: bugs found outside kohau's lane
are documented here, not fixed inline. kohau does not patch kaikai.

## kaikai 0.98.0

### `extern "C" opaque` types cannot appear in `pub` signatures

**Symptom.** An `extern "C" opaque Name` declaration (FFI v2,
kaikai 0.91.0+) is lowered to a *private* nominal type
(`lower_extern_opaques` in `stage2/compiler/desugar.kai` emits
`DType(false, ...)`), and the parser accepts no `pub` on an extern
type head (`extern "C" pub opaque` is not a form; the comment in
`parse_decl` says type heads take no `pub`). Any `pub fn` whose
signature mentions the opaque type is then rejected by the
pub-signature validator:

```
error: pub fn `open` exposes non-pub type `Db` from module `sqlite`;
       mark `Db` as `pub` or remove it from the signature
```

There is no way to mark it `pub`, so opaque handles only work in
signatures that are module-private — i.e. single-module programs
(the shape the FFI v2 fixtures exercise).

**Impact on kohau.** Blocks the natural FFI v2 migration of
`kohau.sqlite`'s handles to `extern "C" opaque Db / Stmt`: the
module's entire `pub` surface (`open`, `exec`, `prepare`, ...)
names the handle types.

**Workaround.** Nominal wrapper records (`pub type Db = { raw: Int }`,
same for `Stmt`) over the shim's pointer-as-`Int` representation.
This delivers the typed surface (`Db` ≠ `Stmt` ≠ `Int`,
`Option`-returning constructors) that opaque handles would give;
what it does not deliver is unforgeability — a caller can construct
`Db { raw: n }` from an arbitrary integer (verified: record
construction and field access work cross-module). When opaque
extern types can cross a `pub` boundary, the swap is internal to
`kohau/sqlite.kai`; the public surface does not change.

**Status.** Open upstream, not yet filed. Repro: any two-module
package where module A declares `extern "C" opaque T` and a
`pub fn` mentioning `T`.

### Re-verification of the 0.86.1 entries

- **`awk` stderr noise on FFI builds** — still reproduces on
  0.98.0, unchanged (once per `kai build` with custom `CFLAGS`;
  cosmetic, tier1 green). See the full entry below.
- **`#[unstable]` warnings firing transitively** — no longer
  observed on 0.98.0: a clean `make tier0` emits no unstable
  warnings. Considered resolved upstream.

## kaikai 0.86.1

### `kai build` emits `awk` stderr noise on FFI fixtures

**Symptom.** Building a fixture that passes a custom `CFLAGS`
(the shim sources + `-lsqlite3`, the FFI pattern kohau and henua
both use) prints to stderr, once per build:

```
awk: nonterminated character class [^A-Za-z0-9_.
 source line number 5
 context is
	                  >>>  gsub(/[^A-Za-z0-9_./- <<< ]/, "_", s)
```

**Scope.** Reproduces only for builds that reach the FFI/CFLAGS
path — a trivial `kai build hello.kai` is clean. The `awk` script
is internal to the `kai` wrapper's codegen→`cc` flow (a `gsub` with
an unterminated bracket expression — the `-` inside `[^...]` is
being read as a range operator, and the class is not closed before
the `]`). It is a quoting bug in the wrapper, not in any kohau or
henua source.

**Impact.** Cosmetic only. The fixture compiles, links, runs, and
diffs green; the `awk` invocation fails silently (its output, when
it would have been used, falls back to a no-op) and the build
proceeds. tier1 is **not** blocked — `make tier1` is green at 3
fixtures.

**Workaround.** None needed in kohau. The noise can be filtered at
the call site (`2>&1 | grep -v awk`) for clean logs. Fix belongs in
the kaikai `kai` wrapper's `awk` invocation (close the character
class / escape the literal `-`).

**Status.** Open upstream. File against kaikai when reproduced on a
clean checkout outside this stack.

### `#[unstable]` warnings fire transitively through dependencies

**Symptom.** A package that opts in to a dependency's `#[unstable]`
decls via its own `[unstable]` block in `kai.toml` can still see
unstable warnings on the generated-C path for extern decls reached
*transitively* through that dependency.

**Impact.** Cosmetic; does not block tier1. kohau's in-tree
fixtures opt in (`sqlite`, `client`, `cell` under `[unstable]`), so
direct uses are warning-free.

**Status.** Resolved — no longer observed on kaikai 0.98.0 (see the
re-verification note above). Was also observed and documented by
henua.

## ahu

### `restartable_cell` forces the driver to carry the step's effects

**Symptom.** `ahu.restart.restartable_cell` is built on
`ahu.cell.with_cell`, which shares a single open row variable
between the cell's `step` and its `driver`. A step that carries an
effect (e.g. `Ffi`) therefore forces the driver to carry the *same*
effect. Verified against kaikai 0.86.1 with a fixture (step `/ Ffi`,
driver pure):

```
error: type mismatch in function call
  = note: `restart.restartable_cell` expects:
      (..., (Int, M) -> StepResult[Int] / Actor[M] + Ffi,
            (Pid[M]) -> Unit / Actor[M] + Ffi) -> ...
  = note: found:
      (..., (Int, M) -> ... / Actor[M] + Ffi,
            (Pid[M]) -> Unit / Actor[M]) -> ?t   # driver without Ffi rejected
```

**Impact on kohau.** Blocks supervising `kohau.sqlite.client` with
`restartable_cell`: the client's step carries `Ffi`, so routing it
through `restartable_cell` would re-introduce `Ffi` into the driver
and — transitively — into henua's body above it, breaking the FFI
seal that is the entire point of the cell-wrapped client. This is
*one* of the reasons restart-on-failure is a not-goal for SQLite
(the others are intrinsic to SQLite being embedded — see
`docs/design.md` §*Why restart-on-failure is a not-goal for
SQLite*); it would be the binding blocker for the Postgres driver,
where restart genuinely pays off.

**Workaround.** None in kohau. The fix belongs in ahu: a restart
helper that hand-spawns the cell with `spawn_actor` (the way
kohau's `with_client` already does to seal `Ffi`) rather than
`with_cell`. Until ahu ships that variant, a supervised
FFI-carrying cell cannot keep its effect sealed from the supervised
body.

**Status.** Open against ahu. Not blocking any kohau tier1 fixture
(kohau does not attempt restart in v1).
