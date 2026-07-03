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
#      `kai install` automatically when kai.lock is absent or older
#      than kai.toml.
#   2. Link flags. The shim sources and `-lsqlite3` are passed to the
#      `kai build` driver through `CFLAGS`, which the `kai` wrapper
#      forwards to its underlying `cc`. We use `kai build` as the
#      driver (not raw `kaic2`) because it owns stdlib prelude
#      assembly, package-path resolution (so the `ahu` dep resolves),
#      and edition gates. This mirrors henua's Makefile and the
#      idiomatic `lnds/uira` raylib pattern.
#
# Requirements on the host:
#
#   - `kai` on PATH (`brew install kaikailang-org/kaikai/kaikai`),
#     version 0.91.0+ (git-dep resolution needs 0.83.0+; the FFI v2
#     fixed-width boundary annotations need 0.91.0+).
#   - libsqlite3 development headers + library. macOS Homebrew ships
#     them under `/opt/homebrew/opt/sqlite/`; Linux distros ship them
#     under `/usr/include` and `/usr/lib` typically.

KAI_BIN ?= kai

# SQLite. Override via `make SQLITE_INC=... SQLITE_LIB=...` if the
# installation is somewhere non-standard.
SQLITE_INC := /opt/homebrew/opt/sqlite/include
SQLITE_LIB := /opt/homebrew/opt/sqlite/lib

# The shim ships inside this repo (unlike henua, which fetches it
# from the kohau cache). `-include` brings the shim declarations
# into the generated C; the shim source is appended so `cc` compiles
# + links it in one step; `-lsqlite3` resolves the symbols it calls.
SHIM_C := c/sqlite_shim.c
SHIM_H := c/sqlite_shim.h

KAI_CFLAGS := -std=c99 -O2 -Wno-unused-function -Wno-unused-variable \
              -I$(SQLITE_INC) -include $(SHIM_H) $(SHIM_C) \
              -L$(SQLITE_LIB) -lsqlite3

BUILD = build

# Fixture discovery. Every tests/*.kai is an FFI fixture (they all
# reach libsqlite3, directly via kohau.sqlite or through the cell).
TEST_KAI   = $(wildcard tests/*.kai)
TEST_NAMES = $(patsubst tests/%.kai,%,$(TEST_KAI))
TEST_BINS  = $(addprefix $(BUILD)/,$(TEST_NAMES))

KOHAU_SRC = $(wildcard kohau/*.kai) $(wildcard kohau/sqlite/*.kai)

.PHONY: tier0 tier1 tier1-fixtures clean

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

# Per-fixture build. `kai build` is the driver; the shim sources and
# `-lsqlite3` go through CFLAGS. Depends on kai.lock so a missing or
# stale lockfile triggers an install first.
$(BUILD)/%: tests/%.kai $(KOHAU_SRC) $(SHIM_C) $(SHIM_H) kai.toml kai.lock | $(BUILD)
	CFLAGS="$(KAI_CFLAGS)" $(KAI_BIN) build $< -o $@

# kai.lock — produced by `kai install`. Treat it as a build input so
# fixtures rebuild after a dependency change and an install runs when
# the lockfile is absent or older than kai.toml.
kai.lock: kai.toml
	$(KAI_BIN) install

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
