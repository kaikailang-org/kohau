# Known regressions

Open issues *outside this repository* that block or constrain kohau
against the current kaikai release. Each entry names the upstream
artefact, the observed symptom, the workaround in kohau (if any),
and whether it blocks tier1.

This file follows ahu's convention: bugs found outside kohau's lane
are documented here, not fixed inline. kohau does not patch kaikai.

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

**Status.** Also observed and documented by henua. Open upstream.
