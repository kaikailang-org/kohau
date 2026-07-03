// kohau/c/sqlite_shim.h — forward declarations for the C shim so the
// shim's translation unit is checked against one authoritative set of
// signatures.
//
// Widths match the kaikai-side extern declarations exactly: handles
// and 64-bit values are `int64_t` (kaikai `Int`), SQLite result codes
// and small counts are `int` (kaikai `I32` boundary annotation).

#ifndef KOHAU_SQLITE_SHIM_H
#define KOHAU_SQLITE_SHIM_H

#include <stdint.h>

int64_t     kai_sqlite_open(const char *path);
int         kai_sqlite_close(int64_t handle);
int         kai_sqlite_exec(int64_t handle, const char *sql);
int64_t     kai_sqlite_prepare(int64_t handle, const char *sql);
int         kai_sqlite_bind_text(int64_t stmt_handle, int idx, const char *value);
int         kai_sqlite_bind_int(int64_t stmt_handle, int idx, int64_t value);
int         kai_sqlite_step(int64_t stmt_handle);
int64_t     kai_sqlite_column_int(int64_t stmt_handle, int col);
const char *kai_sqlite_column_text(int64_t stmt_handle, int col);
int         kai_sqlite_column_count(int64_t stmt_handle);
int         kai_sqlite_reset(int64_t stmt_handle);
int         kai_sqlite_finalize(int64_t stmt_handle);
int64_t     kai_sqlite_last_insert_rowid(int64_t handle);
int         kai_sqlite_changes(int64_t handle);
const char *kai_sqlite_errmsg(int64_t handle);

#endif
