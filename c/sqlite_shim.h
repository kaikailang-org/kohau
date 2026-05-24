// kohau/c/sqlite_shim.h — forward declarations for the C shim so the
// kaic2-generated C resolves the `extern "C"` symbols at link time.

#ifndef KOHAU_SQLITE_SHIM_H
#define KOHAU_SQLITE_SHIM_H

long        kai_sqlite_open(const char *path);
int         kai_sqlite_close(long handle);
int         kai_sqlite_exec(long handle, const char *sql);
long        kai_sqlite_prepare(long handle, const char *sql);
int         kai_sqlite_bind_text(long stmt_handle, int idx, const char *value);
int         kai_sqlite_bind_int(long stmt_handle, int idx, long value);
int         kai_sqlite_step(long stmt_handle);
long        kai_sqlite_column_int(long stmt_handle, int col);
const char *kai_sqlite_column_text(long stmt_handle, int col);
int         kai_sqlite_reset(long stmt_handle);
int         kai_sqlite_finalize(long stmt_handle);
long        kai_sqlite_last_insert_rowid(long handle);
int         kai_sqlite_changes(long handle);
const char *kai_sqlite_errmsg(long handle);

#endif
