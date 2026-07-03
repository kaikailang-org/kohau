// kohau/c/sqlite_shim.c — C shim flattening libsqlite3's API to the
// shape kaikai's FFI can bind against.
//
// What the FFI cannot express, and what this shim flattens:
//
//   - out-parameters / pointer-to-T arguments (the `sqlite3 **db`
//     out-parameter on `sqlite3_open`, the `sqlite3_stmt **stmt`
//     out-parameter on `sqlite3_prepare_v2`). The wrapper allocates
//     the output internally and returns it, 0 on failure.
//   - opaque pointers. kaikai's `extern "C" opaque` exists but an
//     opaque type cannot cross a `pub` module boundary today (see
//     docs/known-regressions.md), so handles flow as `int64_t` and
//     the kaikai side wraps them in nominal `Db` / `Stmt` types.
//   - defaulted arguments kaikai has no story for (the length and
//     destructor parameters of `sqlite3_bind_text` — the shim passes
//     -1 / SQLITE_TRANSIENT).
//
// Widths are exact per the FFI v2 boundary contract: handles and
// 64-bit values are `int64_t` (kaikai `Int`), SQLite result codes and
// small counts are `int` (kaikai `I32`). Zero (NULL) means failure
// for handle-returning functions.
//
// Symbol naming: every exported function uses the `kai_sqlite_`
// prefix so it cannot collide with libsqlite3's own symbols at
// link time. The kaikai side declares them with `extern "C" fn
// kai_sqlite_*` and the generated code resolves them unchanged.

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

// ---- lifecycle ----

// Open (or create) the database at `path`. The string ":memory:"
// opens an in-memory DB. Returns the connection handle, or 0 on
// failure.
int64_t kai_sqlite_open(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    return (int64_t)(intptr_t)db;
}

// Close a connection. Returns SQLITE_OK (0) on success. Calling
// close on a 0 handle is a no-op (returns SQLITE_OK).
int kai_sqlite_close(int64_t handle) {
    if (handle == 0) return SQLITE_OK;
    return sqlite3_close((sqlite3 *)(intptr_t)handle);
}

// ---- single-shot execution ----

// Execute one or more SQL statements without bindings or row
// retrieval. Useful for DDL (CREATE/DROP) and bookkeeping. Returns
// SQLITE_OK on success.
int kai_sqlite_exec(int64_t handle, const char *sql) {
    sqlite3 *db = (sqlite3 *)(intptr_t)handle;
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

// ---- prepared statements ----

// Compile `sql` into a statement handle. Returns the handle, or 0 on
// failure (the connection's errmsg carries the cause).
int64_t kai_sqlite_prepare(int64_t handle, const char *sql) {
    sqlite3 *db = (sqlite3 *)(intptr_t)handle;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return 0;
    }
    return (int64_t)(intptr_t)stmt;
}

// Bind a text parameter (1-indexed per sqlite3_bind_text contract).
// SQLITE_TRANSIENT (-1) tells sqlite to copy the value, so the
// kaikai-side string can be dropped immediately after the call.
int kai_sqlite_bind_text(int64_t stmt_handle, int idx, const char *value) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

// Bind an integer parameter (1-indexed). 64-bit since kaikai's Int
// is 64-bit; libsqlite3's int64 binding takes care of narrowing.
int kai_sqlite_bind_int(int64_t stmt_handle, int idx, int64_t value) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return sqlite3_bind_int64(stmt, idx, (sqlite3_int64)value);
}

// Advance the statement. Returns:
//   - SQLITE_ROW  (100): a row is available; read with column_*
//   - SQLITE_DONE (101): no more rows
//   - SQLITE_BUSY (5), SQLITE_ERROR (1), etc. on failure
int kai_sqlite_step(int64_t stmt_handle) {
    return sqlite3_step((sqlite3_stmt *)(intptr_t)stmt_handle);
}

// Read an integer column from the current row (0-indexed).
// libsqlite3 returns 0 if the column is NULL or non-numeric.
int64_t kai_sqlite_column_int(int64_t stmt_handle, int col) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return (int64_t)sqlite3_column_int64(stmt, col);
}

// Read a text column. Returns a pointer into sqlite's internal
// buffer; valid until the next step / finalize on the same
// statement. The kaikai-side String copies the bytes out at the
// boundary.
const char *kai_sqlite_column_text(int64_t stmt_handle, int col) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return (const char *)sqlite3_column_text(stmt, col);
}

// Number of columns in the result set of a prepared statement.
// Valid after prepare (before the first step), so callers can
// discover how many columns a SELECT returns without declaring it —
// `SELECT *` and explicit column lists both work. Returns 0 for
// statements that produce no result set (DDL, INSERT).
int kai_sqlite_column_count(int64_t stmt_handle) {
    return sqlite3_column_count((sqlite3_stmt *)(intptr_t)stmt_handle);
}

// Reset a prepared statement to its initial state so it can be
// re-stepped (with the same bindings) or re-bound. Returns the
// SQLite result code from the prior execution, not a new error.
int kai_sqlite_reset(int64_t stmt_handle) {
    return sqlite3_reset((sqlite3_stmt *)(intptr_t)stmt_handle);
}

// Free the statement. Returns the SQLite result code; safe to call
// on a 0 handle (no-op).
int kai_sqlite_finalize(int64_t stmt_handle) {
    if (stmt_handle == 0) return SQLITE_OK;
    return sqlite3_finalize((sqlite3_stmt *)(intptr_t)stmt_handle);
}

// ---- diagnostics ----

// rowid of the most recently inserted row. Returns 0 if no insert
// has happened on this connection.
int64_t kai_sqlite_last_insert_rowid(int64_t handle) {
    sqlite3 *db = (sqlite3 *)(intptr_t)handle;
    return (int64_t)sqlite3_last_insert_rowid(db);
}

// Number of rows changed by the most recent INSERT / UPDATE /
// DELETE on this connection.
int kai_sqlite_changes(int64_t handle) {
    return sqlite3_changes((sqlite3 *)(intptr_t)handle);
}

// Human-readable error message from the most recent failed call on
// this connection. Returns a pointer into sqlite's internal buffer;
// valid until the next operation on the connection.
const char *kai_sqlite_errmsg(int64_t handle) {
    return sqlite3_errmsg((sqlite3 *)(intptr_t)handle);
}
