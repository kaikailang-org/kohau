// kohau/c/sqlite_shim.c — C shim flattening libsqlite3's API to the
// shape kaikai FFI v1 can bind against.
//
// FFI v1 (kaikai 0.72.x) does NOT support:
//   - out-parameters / pointer-to-T arguments
//   - opaque pointers by value
//   - struct-by-value
//   - callbacks from C back into kaikai
//   - variadic C functions
//
// libsqlite3's native API uses several of these (most notably the
// `sqlite3 **db` out-parameter on `sqlite3_open` and the
// `sqlite3_stmt **stmt` out-parameter on `sqlite3_prepare_v2`).
// This shim translates each shape:
//
//   - Opaque handles (`sqlite3 *`, `sqlite3_stmt *`) flow through as
//     `long`. Zero (NULL) means failure; non-zero is a live handle.
//     The kaikai side declares these as `Int`.
//   - Out-parameters are eliminated: the wrapper allocates the
//     output internally and returns it (with NULL on failure).
//     The kaikai side checks `if h == 0`.
//   - Error codes (SQLITE_OK == 0, etc.) come back as `int` returns
//     on the void-returning wrappers. The kaikai side checks
//     `if rc != 0`.
//
// Symbol naming: every exported function uses the `kai_sqlite_`
// prefix so it cannot collide with libsqlite3's own symbols at
// link time. The kaikai side declares them with `extern "C" fn
// kai_sqlite_*` and the kaic2-generated C resolves them
// unchanged.

#include <sqlite3.h>
#include <stddef.h>

// ---- lifecycle ----

// Open (or create) the database at `path`. The string ":memory:"
// opens an in-memory DB. Returns the connection handle as long,
// or 0 on failure.
long kai_sqlite_open(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    return (long)db;
}

// Close a connection. Returns SQLITE_OK (0) on success. Calling
// close on a 0 handle is a no-op (returns SQLITE_OK).
int kai_sqlite_close(long handle) {
    if (handle == 0) return SQLITE_OK;
    sqlite3 *db = (sqlite3 *)handle;
    return sqlite3_close(db);
}

// ---- single-shot execution ----

// Execute one or more SQL statements without bindings or row
// retrieval. Useful for DDL (CREATE/DROP) and bookkeeping. Returns
// SQLITE_OK on success.
int kai_sqlite_exec(long handle, const char *sql) {
    sqlite3 *db = (sqlite3 *)handle;
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

// ---- prepared statements ----

// Compile `sql` into a statement handle. Returns the handle as
// long, or 0 on failure (no row retrieval, no error info — v1 is
// the minimum useful surface). Future versions will expose the
// last-error string from the connection.
long kai_sqlite_prepare(long handle, const char *sql) {
    sqlite3 *db = (sqlite3 *)handle;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return 0;
    }
    return (long)stmt;
}

// Bind a text parameter (1-indexed per sqlite3_bind_text contract).
// SQLITE_TRANSIENT (-1) tells sqlite to copy the value, so the
// kaikai-side string can be dropped immediately after the call.
int kai_sqlite_bind_text(long stmt_handle, int idx, const char *value) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

// Bind an integer parameter (1-indexed). 64-bit since kaikai's Int
// is 64-bit; libsqlite3's int64 binding takes care of narrowing.
int kai_sqlite_bind_int(long stmt_handle, int idx, long value) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return sqlite3_bind_int64(stmt, idx, (sqlite3_int64)value);
}

// Advance the statement. Returns:
//   - SQLITE_ROW  (100): a row is available; read with column_*
//   - SQLITE_DONE (101): no more rows
//   - SQLITE_BUSY (5), SQLITE_ERROR (1), etc. on failure
int kai_sqlite_step(long stmt_handle) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return sqlite3_step(stmt);
}

// Read an integer column from the current row (0-indexed).
// libsqlite3 returns 0 if the column is NULL or non-numeric.
long kai_sqlite_column_int(long stmt_handle, int col) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return (long)sqlite3_column_int64(stmt, col);
}

// Read a text column. Returns a pointer into sqlite's internal
// buffer; valid until the next step / finalize on the same
// statement. The kaikai-side wrapper copies the bytes out before
// the next operation.
const char *kai_sqlite_column_text(long stmt_handle, int col) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return (const char *)sqlite3_column_text(stmt, col);
}

// Number of columns in the result set of a prepared statement.
// Valid after prepare (before the first step), so the cell can
// discover how many columns a SELECT returns without the caller
// declaring it — `SELECT *` and explicit column lists both work.
// Returns 0 for statements that produce no result set (DDL, INSERT).
int kai_sqlite_column_count(long stmt_handle) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return sqlite3_column_count(stmt);
}

// Reset a prepared statement to its initial state so it can be
// re-stepped (with the same bindings) or re-bound. Returns the
// SQLite result code from the prior execution, not a new error.
int kai_sqlite_reset(long stmt_handle) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return sqlite3_reset(stmt);
}

// Free the statement. Returns the SQLite result code; safe to call
// on a 0 handle (no-op).
int kai_sqlite_finalize(long stmt_handle) {
    if (stmt_handle == 0) return SQLITE_OK;
    sqlite3_stmt *stmt = (sqlite3_stmt *)stmt_handle;
    return sqlite3_finalize(stmt);
}

// ---- diagnostics ----

// rowid of the most recently inserted row. Returns 0 if no insert
// has happened on this connection.
long kai_sqlite_last_insert_rowid(long handle) {
    sqlite3 *db = (sqlite3 *)handle;
    return (long)sqlite3_last_insert_rowid(db);
}

// Number of rows changed by the most recent INSERT / UPDATE /
// DELETE on this connection.
int kai_sqlite_changes(long handle) {
    sqlite3 *db = (sqlite3 *)handle;
    return sqlite3_changes(db);
}

// Human-readable error message from the most recent failed call on
// this connection. Returns a pointer into sqlite's internal buffer;
// valid until the next operation on the connection.
const char *kai_sqlite_errmsg(long handle) {
    sqlite3 *db = (sqlite3 *)handle;
    return sqlite3_errmsg(db);
}
