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
#include <stdlib.h>
#include <string.h>

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

// Bind a floating-point parameter (1-indexed). kaikai's `Real` is a
// double, and so is SQLite's REAL affinity, so this is a straight
// pass-through.
int kai_sqlite_bind_double(int64_t stmt_handle, int idx, double value) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return sqlite3_bind_double(stmt, idx, value);
}

// Bind SQL NULL to a parameter (1-indexed). Distinct from binding an
// empty string: `NULL = ''` is NULL in SQL, and `IS NULL` only
// matches this.
int kai_sqlite_bind_null(int64_t stmt_handle, int idx) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return sqlite3_bind_null(stmt, idx);
}

// Decode one hex digit, or -1 if the character is not one.
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Bind a BLOB parameter (1-indexed) given its **hex encoding** — see
// the header on why bytes cross the boundary hex-encoded. Returns
// SQLITE_MISUSE for input that is not an even-length run of hex
// digits, which is a caller bug rather than a database error.
//
// SQLITE_TRANSIENT makes SQLite copy the bytes, so the scratch buffer
// is freed before returning.
int kai_sqlite_bind_blob_hex(int64_t stmt_handle, int idx, const char *hex) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    if (hex == NULL) return SQLITE_MISUSE;

    size_t hexlen = strlen(hex);
    if (hexlen % 2 != 0) return SQLITE_MISUSE;
    size_t n = hexlen / 2;

    // A zero-length BLOB is legal and distinct from NULL. malloc(0)
    // may return NULL, so bind it without allocating.
    if (n == 0) return sqlite3_bind_blob(stmt, idx, "", 0, SQLITE_TRANSIENT);

    unsigned char *buf = (unsigned char *)malloc(n);
    if (buf == NULL) return SQLITE_NOMEM;

    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            free(buf);
            return SQLITE_MISUSE;
        }
        buf[i] = (unsigned char)((hi << 4) | lo);
    }

    int rc = sqlite3_bind_blob(stmt, idx, buf, (int)n, SQLITE_TRANSIENT);
    free(buf);
    return rc;
}

// Clear every binding on the statement, resetting them to NULL.
// Needed when a cached statement is reused for a different call: a
// parameter left bound from the previous execution would otherwise
// silently carry over.
int kai_sqlite_clear_bindings(int64_t stmt_handle) {
    return sqlite3_clear_bindings((sqlite3_stmt *)(intptr_t)stmt_handle);
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
//
// A NULL column yields "" rather than the NULL pointer libsqlite3
// returns: the kaikai `String` boundary copies from the pointer
// unconditionally, so handing it NULL is a segfault. Use column_type
// to tell a NULL column from a stored empty string.
const char *kai_sqlite_column_text(int64_t stmt_handle, int col) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    const unsigned char *text = sqlite3_column_text(stmt, col);
    return text == NULL ? "" : (const char *)text;
}

// Read a floating-point column from the current row (0-indexed).
// libsqlite3 returns 0.0 for a NULL or non-numeric column — use
// column_type to tell a real 0.0 from an absent value.
double kai_sqlite_column_double(int64_t stmt_handle, int col) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;
    return sqlite3_column_double(stmt, col);
}

// Scratch buffer for the hex encoding of a BLOB column. Thread-local
// because a kaikai fiber may be resumed on a different OS thread than
// the one that parked it, and two fibers encoding concurrently would
// otherwise share one buffer.
//
// The lifetime contract matches sqlite3_column_text's: the pointer is
// valid until the next call on the same thread. That is enough — the
// kaikai String copies the bytes out at the FFI boundary before
// anything else runs.
static _Thread_local char *hex_buf = NULL;
static _Thread_local size_t hex_buf_cap = 0;

// Read a BLOB column (0-indexed) as its hex encoding — see the header
// on why bytes cross the boundary encoded. Returns "" for a NULL
// column and for a zero-length BLOB; column_type separates the two.
// Returns NULL only if the buffer cannot be grown.
const char *kai_sqlite_column_blob_hex(int64_t stmt_handle, int col) {
    static const char digits[] = "0123456789abcdef";
    sqlite3_stmt *stmt = (sqlite3_stmt *)(intptr_t)stmt_handle;

    const unsigned char *bytes = (const unsigned char *)sqlite3_column_blob(stmt, col);
    int n = sqlite3_column_bytes(stmt, col);
    if (bytes == NULL || n <= 0) return "";

    size_t need = (size_t)n * 2 + 1;
    if (need > hex_buf_cap) {
        char *grown = (char *)realloc(hex_buf, need);
        // "" rather than NULL: every pointer returned here is copied
        // by the kaikai String boundary without a null check. An
        // allocation failure this size is already fatal territory;
        // crashing the process at the FFI edge is not an improvement.
        if (grown == NULL) return "";
        hex_buf = grown;
        hex_buf_cap = need;
    }

    for (int i = 0; i < n; i++) {
        hex_buf[2 * i] = digits[bytes[i] >> 4];
        hex_buf[2 * i + 1] = digits[bytes[i] & 0x0f];
    }
    hex_buf[2 * n] = '\0';
    return hex_buf;
}

// Storage class of the value in the current row's column (0-indexed):
// SQLITE_INTEGER 1, SQLITE_FLOAT 2, SQLITE_TEXT 3, SQLITE_BLOB 4,
// SQLITE_NULL 5. The only way to tell a NULL from a 0 / "" that the
// column_* readers coerce it to.
int kai_sqlite_column_type(int64_t stmt_handle, int col) {
    return sqlite3_column_type((sqlite3_stmt *)(intptr_t)stmt_handle, col);
}

// Byte length of a TEXT or BLOB column in the current row. Must be
// read after the corresponding column_text / column_blob call, per
// libsqlite3's contract.
int kai_sqlite_column_bytes(int64_t stmt_handle, int col) {
    return sqlite3_column_bytes((sqlite3_stmt *)(intptr_t)stmt_handle, col);
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
    if (handle == 0) return "";
    const char *msg = sqlite3_errmsg((sqlite3 *)(intptr_t)handle);
    return msg == NULL ? "" : msg;
}

// Primary result code of the most recent failed call on this
// connection — the stable, locale-independent classifier (SQLITE_BUSY,
// SQLITE_CONSTRAINT, …), unlike the message text.
int kai_sqlite_errcode(int64_t handle) {
    if (handle == 0) return SQLITE_OK;
    return sqlite3_errcode((sqlite3 *)(intptr_t)handle);
}

// Extended result code of the most recent failed call: the primary
// code in its low 8 bits plus a refinement above them
// (SQLITE_CONSTRAINT_UNIQUE = 2067 vs SQLITE_CONSTRAINT = 19). Which
// constraint was violated is exactly what a caller branches on.
int kai_sqlite_extended_errcode(int64_t handle) {
    if (handle == 0) return SQLITE_OK;
    return sqlite3_extended_errcode((sqlite3 *)(intptr_t)handle);
}
