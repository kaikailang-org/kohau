#!/bin/sh
# Drives tests/pg_client_reconnect: runs the fixture and restarts the
# server underneath it, so the reconnect path is exercised against a
# connection that really dropped rather than a simulated one.
#
# The fixture signals it is ready by creating a table named
# `restart_now`; this script polls for it, restarts the server, and
# drops the table. Usage:
#
#   tests/pg-restart-driver.sh <fixture-binary> <pgdata-dir>
#
# PGHOST / PGDATABASE / PGUSER select the server, as for every other
# postgres fixture. Requires pg_ctl on PATH.

set -e

BIN="$1"
PGDATA_DIR="$2"

if [ -z "$BIN" ] || [ -z "$PGDATA_DIR" ]; then
  echo "usage: $0 <fixture-binary> <pgdata-dir>" >&2
  exit 2
fi

OUT="$(dirname "$BIN")/$(basename "$BIN").raw"

"$BIN" > "$OUT" 2>/dev/null &
FIXTURE_PID=$!

# Wait for the fixture's signal, restart, then clear the signal.
i=0
while [ $i -lt 100 ]; do
  if psql -tAc "SELECT 1 FROM pg_tables WHERE tablename = 'restart_now'" 2>/dev/null | grep -q 1; then
    # Stop and start as two steps with a pause between: a `restart`
    # can complete faster than the fixture polls, so it would never
    # see a failed statement and the reconnect path would go
    # unexercised.
    pg_ctl -D "$PGDATA_DIR" stop -m immediate >/dev/null 2>&1
    sleep 1
    pg_ctl -D "$PGDATA_DIR" start -o "-k $PGHOST -h ''" -l "$PGDATA_DIR/server.log" >/dev/null 2>&1
    j=0
    while [ $j -lt 50 ]; do
      psql -tAc "SELECT 1" >/dev/null 2>&1 && break
      j=$((j + 1))
      sleep 0.2
    done
    psql -qc "DROP TABLE IF EXISTS restart_now" >/dev/null 2>&1 || true
    break
  fi
  i=$((i + 1))
  sleep 0.1
done

wait $FIXTURE_PID
cat "$OUT"
