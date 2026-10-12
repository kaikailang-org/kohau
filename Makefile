# kohau Makefile.
#
# kohau binds libsqlite3 through a C shim (see c/sqlite_shim.{c,h}),
# and — since v0.2 — its cell-wrapped client (`kohau.sqlite.client`)
# depends on the `ahu` package (resolved as a git-dep in kai.toml).
#
# Two consequences for the build:
#
#   1. Dependency resolution. The `ahu` dep must be fetched into the
#      user-level cache and pinned in kai.lock before fixtures that
#      import `kohau.sqlite.client` can compile. `make` runs
#      `kai fetch` automatically when kai.lock is absent or older
#      than kai.toml.
#   2. Link flags. The sqlite shim and `-lsqlite3` are declared in
#      kai.toml's `[native]` table, so `kai build` compiles and links
#      them with no flags from here. The postgres shim still goes
#      through `CFLAGS`, which the `kai` wrapper forwards to its
#      underlying `cc`. We use `kai build` as the driver (not raw
#      `kaic2`) because it owns stdlib prelude assembly, package-path
#      resolution (so the `ahu` dep resolves), and edition gates.
#
# Requirements on the host:
#
#   - `kai` on PATH (`brew install kaikailang-org/kaikai/kaikai`),
#     version 0.91.0+ (git-dep resolution needs 0.83.0+; the FFI v2
#     fixed-width boundary annotations need 0.91.0+).
#   - libsqlite3 development headers + library. macOS Homebrew ships
#     them under `/opt/homebrew/opt/sqlite/`; Linux distros ship them
#     under `/usr/include` and `/usr/lib` typically.
#   - For the postgres tier only: libpq headers + library (`brew
#     install libpq`, or libpq-dev on Debian-alikes) and a reachable
#     server. `tier0`/`tier1` do not need either.

KAI_BIN ?= kai

# Library paths are discovered, not assumed, so the same Makefile
# works on a Homebrew macOS box and a distro-packaged Linux one. Each
# is `?=`, so an explicit `make PG_INC=... PG_LIB=...` still wins.
#
# macOS keeps libpq keg-only under `/opt/homebrew/opt/`, where the
# compiler does not look by default, so the paths must be spelled
# out. On Linux it lands in the default search path, and the
# empty-string case below means "pass no -I/-L at all" rather than a
# bogus directory. SQLite needs none of this: the compiler builds
# that shim through `[native]`, and libsqlite3 is in the default
# search path on both platforms.

# PostgreSQL: `pg_config` ships with libpq itself and is the
# authoritative answer on both platforms; Homebrew's keg is the
# fallback for a macOS box where libpq is installed but not on PATH.
PG_INC ?= $(shell pg_config --includedir 2>/dev/null || echo /opt/homebrew/opt/libpq/include)
PG_LIB ?= $(shell pg_config --libdir 2>/dev/null || echo /opt/homebrew/opt/libpq/lib)

# Expand to nothing when the corresponding path came back empty.
PG_INC_FLAG = $(if $(PG_INC),-isystem $(PG_INC))
PG_LIB_FLAG = $(if $(PG_LIB),-L$(PG_LIB))

# The sqlite shim is declared in kai.toml's `[native]` table, so the
# compiler owns compiling and linking it and nothing about it belongs
# on CFLAGS — passing it here as well would link two copies and fail
# on duplicate symbols.
SHIM_C := c/sqlite_shim.c
SHIM_H := c/sqlite_shim.h

# The postgres shim still rides CFLAGS. `[native]` compiles every
# source it names for every consumer, and it has no way to name a
# library directory, so a declared libpq shim would demand libpq of a
# consumer that only wants sqlite and still fail to link on macOS,
# where the keg is outside the default search path.
PG_SHIM_C := c/postgres_shim.c
PG_SHIM_H := c/postgres_shim.h

# The shim is appended as its own translation unit and includes its
# own header, rather than being force-included into the generated C.
# `-include` would put a system header ahead of the kaikai runtime
# header, which must define its feature-test macros before anything
# else is parsed; on glibc that hides the POSIX declarations the
# runtime needs and the build fails to compile.
PG_CFLAGS = -std=c99 -O2 -Wno-unused-function -Wno-unused-variable \
            $(PG_INC_FLAG) $(PG_SHIM_C) \
            $(PG_LIB_FLAG) -lpq

BUILD = build

# Fixture discovery. `tests/pg_*.kai` bind libpq and need a live
# server; everything else reaches libsqlite3 and runs anywhere. The
# two sets link against different shims, so they are built by
# separate rules and only the sqlite set rides the default tier0/tier1.
# The reconnect fixture restarts the server underneath itself, so it
# cannot share a target with fixtures that assume a stable one — it
# runs under `tier1-pg-reconnect` with a driver script instead.
PG_RESTART_KAI  = tests/pg_client_reconnect.kai
PG_RESTART_NAME = pg_client_reconnect
PG_RESTART_BIN  = $(BUILD)/$(PG_RESTART_NAME)
PG_RESTART_DRV  = tests/pg-restart-driver.sh

PG_TEST_KAI   = $(filter-out $(PG_RESTART_KAI),$(wildcard tests/pg_*.kai))
PG_TEST_NAMES = $(patsubst tests/%.kai,%,$(PG_TEST_KAI))
PG_TEST_BINS  = $(addprefix $(BUILD)/,$(PG_TEST_NAMES))

TEST_KAI   = $(filter-out $(PG_TEST_KAI) $(PG_RESTART_KAI),$(wildcard tests/*.kai))
TEST_NAMES = $(patsubst tests/%.kai,%,$(TEST_KAI))
TEST_BINS  = $(addprefix $(BUILD)/,$(TEST_NAMES))

KOHAU_SRC = $(wildcard kohau/*.kai) $(wildcard kohau/sqlite/*.kai) \
            $(wildcard kohau/postgres/*.kai)

.PHONY: tier0 tier1 tier1-fixtures tier0-pg tier1-pg tier1-pg-fixtures \
        tier1-pg-reconnect clean

tier0: $(TEST_BINS)
	@echo "tier0: kohau modules + $(words $(TEST_BINS)) fixtures compile."

tier1: tier0 tier1-fixtures
	@echo "tier1: $(words $(TEST_BINS)) fixtures pass."

tier1-fixtures: $(TEST_BINS)
	@set -e; \
	for n in $(TEST_NAMES); do \
	  bin="$(BUILD)/$$n"; \
	  exp="tests/$$n.out.expected"; \
	  out="$(BUILD)/$$n.out"; \
	  if [ ! -f "$$exp" ]; then echo "tier1: missing $$exp"; exit 1; fi; \
	  "$$bin" > "$$out"; \
	  diff -u "$$exp" "$$out" || { echo "tier1: $$n FAIL"; exit 1; }; \
	  echo "tier1: $$n OK"; \
	done

# ---- postgres tier ----
#
# Kept out of the default tier0/tier1 because these fixtures need a
# running server, which a bare checkout does not have. They connect
# through libpq's environment (PGHOST / PGDATABASE / PGUSER) with an
# empty conninfo, so the caller points them at any reachable cluster:
#
#   make PGHOST=/tmp/kohau-pg PGDATABASE=kohau_test tier1-pg
#
# Each fixture creates and drops its own tables, so a re-run is
# idempotent against the same database.

tier0-pg: $(PG_TEST_BINS)
	@echo "tier0-pg: kohau.postgres + $(words $(PG_TEST_BINS)) fixtures compile."

tier1-pg: tier0-pg tier1-pg-fixtures
	@echo "tier1-pg: $(words $(PG_TEST_BINS)) fixtures pass."

tier1-pg-fixtures: $(PG_TEST_BINS)
	@set -e; \
	for n in $(PG_TEST_NAMES); do \
	  bin="$(BUILD)/$$n"; \
	  exp="tests/$$n.out.expected"; \
	  out="$(BUILD)/$$n.out"; \
	  if [ ! -f "$$exp" ]; then echo "tier1-pg: missing $$exp"; exit 1; fi; \
	  "$$bin" > "$$out"; \
	  diff -u "$$exp" "$$out" || { echo "tier1-pg: $$n FAIL"; exit 1; }; \
	  echo "tier1-pg: $$n OK"; \
	done

# Reconnect coverage. Separate from tier1-pg because it stops and
# starts the server mid-run, which every other fixture assumes will
# not happen. Needs the data directory as well as the connection
# settings, since it drives pg_ctl:
#
#   make PGHOST=/tmp/kohau-pg PGDATABASE=kohau_test \
#        PGDATA_DIR=/path/to/pgdata tier1-pg-reconnect
#
# The server is left running afterwards, as it was found.
tier1-pg-reconnect: $(PG_RESTART_BIN)
	@if [ -z "$(PGDATA_DIR)" ]; then \
	  echo "tier1-pg-reconnect: set PGDATA_DIR to the server's data directory"; exit 1; \
	fi
	@set -e; \
	exp="tests/$(PG_RESTART_NAME).out.expected"; \
	out="$(BUILD)/$(PG_RESTART_NAME).out"; \
	sh $(PG_RESTART_DRV) $(PG_RESTART_BIN) "$(PGDATA_DIR)" > "$$out"; \
	diff -u "$$exp" "$$out" || { echo "tier1-pg-reconnect: FAIL"; exit 1; }; \
	echo "tier1-pg-reconnect: $(PG_RESTART_NAME) OK"

# Per-fixture build. `kai build` is the driver. Depends on kai.lock so
# a missing or stale lockfile triggers a fetch first. The `pg_%` rule
# is more specific than the catch-all below, so make prefers it for
# the postgres fixtures — they link libpq through CFLAGS, while the
# sqlite ones need no flags at all.
$(BUILD)/pg_%: tests/pg_%.kai $(KOHAU_SRC) $(PG_SHIM_C) $(PG_SHIM_H) kai.toml kai.lock | $(BUILD)
	CFLAGS="$(PG_CFLAGS)" $(KAI_BIN) build $< -o $@

$(BUILD)/%: tests/%.kai $(KOHAU_SRC) $(SHIM_C) $(SHIM_H) kai.toml kai.lock | $(BUILD)
	$(KAI_BIN) build $< -o $@

# kai.lock — produced by `kai fetch`. Treat it as a build input so
# fixtures rebuild after a dependency change and a fetch runs when
# the lockfile is absent or older than kai.toml.
kai.lock: kai.toml
	$(KAI_BIN) fetch

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
