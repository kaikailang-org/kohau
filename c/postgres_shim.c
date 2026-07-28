// kohau/c/postgres_shim.c — C shim flattening libpq's API to the shape
// kaikai's FFI can bind against.
//
// The FFI passes scalars, strings, fixed-width numbers and structs of
// those. It cannot pass an array of strings, which is exactly what
// `PQexecParams`'s `paramValues` argument is. The shim closes that gap
// by keeping the parameter vector on the C side: the kaikai caller
// pushes binds one at a time (`params_push_text` / `params_push_null`),
// then calls `exec_params`, which hands the accumulated vector to
// `PQexecParams`. Values therefore travel out-of-band and reach the
// server as parameters, never as SQL text — the shim exposes no way to
// splice a value into a statement.
//
// Other shapes translated here:
//   - Opaque handles (`PGconn *`, `PGresult *`) flow through as
//     `int64_t`. Zero (NULL) means failure; the kaikai side declares
//     these as `Int` and wraps them in nominal `Conn` / `Res` types.
//   - `PQresultStatus` returns an `ExecStatusType` enum; we return it
//     as `int` and the kaikai side names the two success values.
//
// Unlike SQLite's prepare/step/finalize cursor, `PQexecParams` runs the
// statement and materialises the whole result set into one `PGresult`.
// Row/column access is by integer index, which maps directly onto
// kohau's already-materialised `[[String]]` reply.
//
// The parameter buffer is per-connection and not thread-safe: one
// push/exec sequence must complete before the next begins on the same
// connection. kohau's cell owns the connection, which serialises access.

#include <libpq-fe.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ---- parameter accumulation ----

// Parameters are held per connection. A fixed ceiling avoids a growth
// path in the shim; statements needing more binds than this are beyond
// what kohau's surface targets.
#define KAI_PG_MAX_PARAMS 128
#define KAI_PG_MAX_CONNS 64

typedef struct {
    PGconn *conn;
    char   *values[KAI_PG_MAX_PARAMS];
    int     count;
    int     overflow;
} kai_pg_slot;

static kai_pg_slot kai_pg_slots[KAI_PG_MAX_CONNS];

static kai_pg_slot *kai_pg_slot_for(PGconn *conn) {
    if (conn == NULL) return NULL;
    for (int i = 0; i < KAI_PG_MAX_CONNS; i++) {
        if (kai_pg_slots[i].conn == conn) return &kai_pg_slots[i];
    }
    return NULL;
}

static kai_pg_slot *kai_pg_slot_claim(PGconn *conn) {
    if (conn == NULL) return NULL;
    kai_pg_slot *existing = kai_pg_slot_for(conn);
    if (existing != NULL) return existing;
    for (int i = 0; i < KAI_PG_MAX_CONNS; i++) {
        if (kai_pg_slots[i].conn == NULL) {
            kai_pg_slots[i].conn = conn;
            kai_pg_slots[i].count = 0;
            kai_pg_slots[i].overflow = 0;
            return &kai_pg_slots[i];
        }
    }
    return NULL;
}

static void kai_pg_slot_clear(kai_pg_slot *slot) {
    if (slot == NULL) return;
    for (int i = 0; i < slot->count; i++) {
        free(slot->values[i]);
        slot->values[i] = NULL;
    }
    slot->count = 0;
    slot->overflow = 0;
}

// ---- lifecycle ----

// Open a connection using a libpq conninfo / URI string
// (e.g. "host=/tmp port=5432 dbname=kohau" or
// "postgresql://user@host/db"). Returns the handle even when the
// connection FAILED: `PQconnectdb` returns a non-NULL object on a
// parseable string, and the error text is only reachable through it.
// The kaikai side checks `status` and reads the message before
// discarding a bad handle. Only a NULL return (OOM) maps to 0.
int64_t kai_pg_connect(const char *conninfo) {
    PGconn *conn = PQconnectdb(conninfo);
    if (conn == NULL) return 0;
    if (kai_pg_slot_claim(conn) == NULL) {
        PQfinish(conn);
        return 0;
    }
    return (int64_t)conn;
}

// Connection status. Returns 0 (CONNECTION_OK) when usable, non-zero
// otherwise. Safe on a 0 handle (reports bad).
int kai_pg_status(int64_t conn_handle) {
    if (conn_handle == 0) return CONNECTION_BAD;
    return (int)PQstatus((PGconn *)conn_handle);
}

// Transaction state: 0 idle, 1 active, 2 inside a transaction block,
// 3 inside a FAILED transaction block (every statement rejected until
// it ends), 4 unknown. Safe on a 0 handle (reports unknown).
int kai_pg_transaction_status(int64_t conn_handle) {
    if (conn_handle == 0) return PQTRANS_UNKNOWN;
    return (int)PQtransactionStatus((PGconn *)conn_handle);
}

// Close the connection, releasing any accumulated parameters first.
// Always returns 0. Safe on a 0 handle (no-op).
int kai_pg_finish(int64_t conn_handle) {
    if (conn_handle == 0) return 0;
    PGconn *conn = (PGconn *)conn_handle;
    kai_pg_slot *slot = kai_pg_slot_for(conn);
    if (slot != NULL) {
        kai_pg_slot_clear(slot);
        slot->conn = NULL;
    }
    PQfinish(conn);
    return 0;
}

// Human-readable error message from the most recent failed operation on
// this connection. Points into libpq's buffer; the kaikai String copies
// the bytes out at the boundary. Empty when there is no error.
const char *kai_pg_error_message(int64_t conn_handle) {
    if (conn_handle == 0) return "";
    return PQerrorMessage((PGconn *)conn_handle);
}

// ---- parameter binding ----

// Discard any accumulated parameters. Call before building a new bind
// list. Returns 0 on success, 1 when the handle has no slot.
int kai_pg_params_reset(int64_t conn_handle) {
    if (conn_handle == 0) return 1;
    kai_pg_slot *slot = kai_pg_slot_for((PGconn *)conn_handle);
    if (slot == NULL) return 1;
    kai_pg_slot_clear(slot);
    return 0;
}

// Append a text parameter, binding `$n` where n is the 1-based push
// order. The value is copied, so the caller's string may be dropped
// immediately. Returns 0 on success, 1 on a bad handle or when the
// parameter ceiling is reached — an overflow is latched so the
// subsequent `exec_params` refuses to run a statement with silently
// missing binds.
int kai_pg_params_push_text(int64_t conn_handle, const char *value) {
    if (conn_handle == 0) return 1;
    kai_pg_slot *slot = kai_pg_slot_for((PGconn *)conn_handle);
    if (slot == NULL) return 1;
    if (slot->count >= KAI_PG_MAX_PARAMS) {
        slot->overflow = 1;
        return 1;
    }
    if (value == NULL) {
        slot->values[slot->count] = NULL;
        slot->count++;
        return 0;
    }
    size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        slot->overflow = 1;
        return 1;
    }
    memcpy(copy, value, len + 1);
    slot->values[slot->count] = copy;
    slot->count++;
    return 0;
}

// Append a SQL NULL parameter. Returns 0 on success, 1 otherwise.
int kai_pg_params_push_null(int64_t conn_handle) {
    if (conn_handle == 0) return 1;
    kai_pg_slot *slot = kai_pg_slot_for((PGconn *)conn_handle);
    if (slot == NULL) return 1;
    if (slot->count >= KAI_PG_MAX_PARAMS) {
        slot->overflow = 1;
        return 1;
    }
    slot->values[slot->count] = NULL;
    slot->count++;
    return 0;
}

// Number of parameters accumulated so far. 0 on a bad handle.
int kai_pg_params_count(int64_t conn_handle) {
    if (conn_handle == 0) return 0;
    kai_pg_slot *slot = kai_pg_slot_for((PGconn *)conn_handle);
    if (slot == NULL) return 0;
    return slot->count;
}

// ---- execution ----

// Execute `sql` with the accumulated parameters bound to `$1..$n` in
// push order, then clear them. All values are sent as text
// (paramTypes / paramLengths / paramFormats NULL, resultFormat 0), so
// the server infers types and every value comes back as a string —
// what kohau's `[[String]]` reply wants.
//
// Returns a result handle, or 0 when the parameter vector overflowed
// (running the statement with missing binds could change its meaning)
// or on a NULL result. A non-zero handle may still describe a *failed*
// statement; the kaikai side checks `result_status`. Every non-zero
// handle must be matched by one `clear`.
int64_t kai_pg_exec_params(int64_t conn_handle, const char *sql) {
    if (conn_handle == 0) return 0;
    PGconn *conn = (PGconn *)conn_handle;
    kai_pg_slot *slot = kai_pg_slot_for(conn);
    if (slot == NULL) return 0;
    if (slot->overflow) {
        kai_pg_slot_clear(slot);
        return 0;
    }
    PGresult *res = PQexecParams(conn, sql, slot->count, NULL,
                                 (const char *const *)slot->values,
                                 NULL, NULL, 0);
    kai_pg_slot_clear(slot);
    return (int64_t)res;
}

// ---- result inspection ----

// Result status as a raw `ExecStatusType` int. The kaikai side names
// PGRES_COMMAND_OK (no-row success) and PGRES_TUPLES_OK (rows
// available) and treats every other value as an error. Safe on a 0
// handle (reports PGRES_FATAL_ERROR).
int kai_pg_result_status(int64_t res_handle) {
    if (res_handle == 0) return PGRES_FATAL_ERROR;
    return (int)PQresultStatus((PGresult *)res_handle);
}

// Statement-level error message — more specific than the
// connection-level one. Empty on success or a 0 handle.
const char *kai_pg_result_error_message(int64_t res_handle) {
    if (res_handle == 0) return "";
    return PQresultErrorMessage((PGresult *)res_handle);
}

// Five-character SQLSTATE for a failed statement (e.g. "23505" for a
// unique violation). Stable across server versions and locales, unlike
// the message text, so callers can branch on the class of failure.
// Empty when the field is absent or on a 0 handle.
const char *kai_pg_result_sqlstate(int64_t res_handle) {
    if (res_handle == 0) return "";
    const char *state = PQresultErrorField((PGresult *)res_handle,
                                           PG_DIAG_SQLSTATE);
    return state == NULL ? "" : state;
}

// The command tag the server actually executed ("INSERT 0 1",
// "SELECT 3", "COMMIT", "ROLLBACK"). Load-bearing for COMMIT: a
// commit issued on an aborted transaction succeeds at the protocol
// level but reports "ROLLBACK", so the tag is the only way to tell a
// real commit from a silently downgraded one. Empty on a 0 handle.
const char *kai_pg_cmd_status(int64_t res_handle) {
    if (res_handle == 0) return "";
    const char *tag = PQcmdStatus((PGresult *)res_handle);
    return tag == NULL ? "" : tag;
}

// Rows affected by an INSERT / UPDATE / DELETE, as the decimal string
// libpq reports (e.g. "3"). Empty for statements that affect no rows.
// The kaikai side parses it to Int.
const char *kai_pg_cmd_tuples(int64_t res_handle) {
    if (res_handle == 0) return "";
    return PQcmdTuples((PGresult *)res_handle);
}

// Number of result rows. 0 for no-row statements or a 0 handle.
int kai_pg_ntuples(int64_t res_handle) {
    if (res_handle == 0) return 0;
    return PQntuples((PGresult *)res_handle);
}

// Number of result columns. 0 for no-row statements or a 0 handle.
int kai_pg_nfields(int64_t res_handle) {
    if (res_handle == 0) return 0;
    return PQnfields((PGresult *)res_handle);
}

// Name of column `col` (0-indexed). Empty when out of range.
const char *kai_pg_fname(int64_t res_handle, int col) {
    if (res_handle == 0) return "";
    const char *name = PQfname((PGresult *)res_handle, col);
    return name == NULL ? "" : name;
}

// Value at (row, col) as text. libpq returns "" for a SQL NULL, so
// callers that must distinguish NULL from empty-string use
// `get_is_null`. Valid until the result is cleared; copied out at the
// FFI boundary.
const char *kai_pg_get_value(int64_t res_handle, int row, int col) {
    if (res_handle == 0) return "";
    const char *v = PQgetvalue((PGresult *)res_handle, row, col);
    return v == NULL ? "" : v;
}

// 1 if the value at (row, col) is SQL NULL, 0 otherwise.
int kai_pg_get_is_null(int64_t res_handle, int row, int col) {
    if (res_handle == 0) return 0;
    return PQgetisnull((PGresult *)res_handle, row, col);
}

// Free a result. Always returns 0. Safe on a 0 handle (no-op). Every
// non-zero `exec_params` handle must be matched by one `clear`.
int kai_pg_clear(int64_t res_handle) {
    if (res_handle == 0) return 0;
    PQclear((PGresult *)res_handle);
    return 0;
}
