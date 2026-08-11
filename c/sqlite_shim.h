// kohau/c/sqlite_shim.h — forward declarations for the C shim so the
// shim's translation unit is checked against one authoritative set of
// signatures.
//
// Widths match the kaikai-side extern declarations exactly: handles
// and 64-bit values are `int64_t` (kaikai `Int`), SQLite result codes
// and small counts are `int` (kaikai `I32` boundary annotation), and
// floating-point values are `double` (kaikai `Real`).
//
// BLOBs cross the boundary **hex-encoded**. kaikai's `String` maps to
// `const char *` and is NUL-terminated, so raw bytes would truncate
// at the first zero — which arbitrary binary data contains. The hex
// pair is the encoding, not the storage: the shim decodes on bind and
// encodes on read, so the value stored in SQLite is a true BLOB.

#ifndef KOHAU_SQLITE_SHIM_H
#define KOHAU_SQLITE_SHIM_H

#include <stdint.h>

int64_t     kai_sqlite_open(const char *path);
int         kai_sqlite_close(int64_t handle);
int         kai_sqlite_exec(int64_t handle, const char *sql);
int64_t     kai_sqlite_prepare(int64_t handle, const char *sql);
int         kai_sqlite_bind_text(int64_t stmt_handle, int idx, const char *value);
int         kai_sqlite_bind_int(int64_t stmt_handle, int idx, int64_t value);
int         kai_sqlite_bind_double(int64_t stmt_handle, int idx, double value);
int         kai_sqlite_bind_null(int64_t stmt_handle, int idx);
int         kai_sqlite_bind_blob_hex(int64_t stmt_handle, int idx, const char *hex);
int         kai_sqlite_clear_bindings(int64_t stmt_handle);
int         kai_sqlite_step(int64_t stmt_handle);
int64_t     kai_sqlite_column_int(int64_t stmt_handle, int col);
const char *kai_sqlite_column_text(int64_t stmt_handle, int col);
double      kai_sqlite_column_double(int64_t stmt_handle, int col);
const char *kai_sqlite_column_blob_hex(int64_t stmt_handle, int col);
int         kai_sqlite_column_type(int64_t stmt_handle, int col);
int         kai_sqlite_column_bytes(int64_t stmt_handle, int col);
int         kai_sqlite_column_count(int64_t stmt_handle);
int         kai_sqlite_reset(int64_t stmt_handle);
int         kai_sqlite_finalize(int64_t stmt_handle);
int64_t     kai_sqlite_last_insert_rowid(int64_t handle);
int         kai_sqlite_changes(int64_t handle);
const char *kai_sqlite_errmsg(int64_t handle);
int         kai_sqlite_errcode(int64_t handle);
int         kai_sqlite_extended_errcode(int64_t handle);

#endif
