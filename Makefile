# kohau Makefile.
#
# kohau's `sqlite` module binds libsqlite3 through a C shim (see
# c/sqlite_shim.{c,h}). `kai build` does not currently expose
# link-flag injection, so we replicate the upstream flow that the
# kaikai stage2 Makefile uses for FFI fixtures:
#
#   1. `kaic2 <fixture>.kai > <fixture>.c`   — emit C
#   2. `cc -include shim.h ... -lsqlite3`    — link manually
#
# Requirements on the host:
#
#   - `kai` and `kaic2` on PATH (Homebrew install lands kaic2 in
#     `<prefix>/libexec/libexec/kaikai/kaic2`; this Makefile asks
#     the user-facing `kai` wrapper for its install root).
#   - libsqlite3 development headers + library. macOS Homebrew
#     ships them under `/opt/homebrew/opt/sqlite/`; Linux distros
#     ship them under `/usr/include` and `/usr/lib` typically.

# Locate kaic2 and the kaikai runtime headers via the `kai` wrapper.
# The wrapper is a thin shell script; we extract its exec path and
# derive the include + libexec roots from it.
KAI_BIN     := $(shell command -v kai)
KAI_PREFIX  := $(shell awk -F'"' '/^exec "/ { print $$2; exit }' "$(KAI_BIN)" | xargs -I{} dirname {} | xargs -I{} dirname {})
KAIC2       := $(KAI_PREFIX)/libexec/kaikai/kaic2
RUNTIME_INC := $(KAI_PREFIX)/share/kaikai/include

# SQLite. Override via `make SQLITE_INC=... SQLITE_LIB=...` if
# the installation is somewhere non-standard.
SQLITE_INC := /opt/homebrew/opt/sqlite/include
SQLITE_LIB := /opt/homebrew/opt/sqlite/lib

CC ?= cc

BUILD = build

# Fixture discovery.
TEST_KAI   = $(wildcard tests/*.kai)
TEST_NAMES = $(patsubst tests/%.kai,%,$(TEST_KAI))
TEST_BINS  = $(addprefix $(BUILD)/,$(TEST_NAMES))

# The shim object — every fixture links against it.
SHIM_OBJ = $(BUILD)/sqlite_shim.o

KOHAU_SRC = $(wildcard kohau/*.kai)

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

# Shim object — compiled once, linked into every fixture.
$(SHIM_OBJ): c/sqlite_shim.c c/sqlite_shim.h | $(BUILD)
	$(CC) -c c/sqlite_shim.c -I$(SQLITE_INC) -o $@

# Per-fixture build. Emit C from the kaikai source, then link with
# the shim and libsqlite3.
$(BUILD)/%: tests/%.kai $(KOHAU_SRC) $(SHIM_OBJ) kai.toml | $(BUILD)
	$(KAIC2) --path . $< > $(BUILD)/$*.c
	$(CC) -I$(RUNTIME_INC) -I$(SQLITE_INC) -include c/sqlite_shim.h \
	  $(BUILD)/$*.c $(SHIM_OBJ) \
	  -L$(SQLITE_LIB) -lsqlite3 \
	  -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
