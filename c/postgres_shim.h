// kohau/c/postgres_shim.h — forward declarations for the C shim so the
// shim's translation unit is checked against one authoritative set of
// signatures.
//
// Widths match the kaikai-side extern declarations exactly: handles are
// `int64_t` (kaikai `Int`), libpq status codes and small counts are
// `int` (kaikai `I32` boundary annotation). Every symbol carries the
// `kai_pg_` prefix so it cannot collide with libpq's own `PQ*` symbols.

#ifndef KOHAU_POSTGRES_SHIM_H
#define KOHAU_POSTGRES_SHIM_H

#include <stdint.h>

int64_t     kai_pg_connect(const char *conninfo);
int         kai_pg_status(int64_t conn_handle);
int         kai_pg_transaction_status(int64_t conn_handle);
int         kai_pg_finish(int64_t conn_handle);
const char *kai_pg_error_message(int64_t conn_handle);

int         kai_pg_params_reset(int64_t conn_handle);
int         kai_pg_params_push_text(int64_t conn_handle, const char *value);
int         kai_pg_params_push_null(int64_t conn_handle);
int         kai_pg_params_count(int64_t conn_handle);

int64_t     kai_pg_exec_params(int64_t conn_handle, const char *sql);

int         kai_pg_result_status(int64_t res_handle);
const char *kai_pg_result_error_message(int64_t res_handle);
const char *kai_pg_result_sqlstate(int64_t res_handle);
const char *kai_pg_cmd_status(int64_t res_handle);
const char *kai_pg_cmd_tuples(int64_t res_handle);
int         kai_pg_ntuples(int64_t res_handle);
int         kai_pg_nfields(int64_t res_handle);
const char *kai_pg_fname(int64_t res_handle, int col);
const char *kai_pg_get_value(int64_t res_handle, int row, int col);
int         kai_pg_get_is_null(int64_t res_handle, int row, int col);
int         kai_pg_clear(int64_t res_handle);

#endif
