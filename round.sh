#!/usr/bin/env bash
#
# One full round, end to end, on a real tree.
#
#   ./round.sh            index $TEST_ROOT, serve it, drive a real client session
#   ./round.sh /usr/share index a bigger subtree
#   ./round.sh -v         echo every line of the session
#
# This is not a test -- test.sh and test_etp.sh are. It is the demonstration that
# the three layers fit together on data nobody curated: scan a real filesystem,
# build the derived indexes, serve ETP, and walk a session shaped like the one the
# ETP client performs (connect, browse, drill down, search, page, sort,
# disconnect), printing the phase timings at each step.
# It is also where the numbers in README.md and docs/design.md come from, so that
# those tables cannot drift away from the code without someone noticing.

set -u
cd "$(dirname "$0")"

BIN=./esidx
PROBE=./etp-probe
TEST_ROOT=${1:-/etc}
[ "${1:-}" = "-v" ] && { VERBOSE=-v 2>&1; TEST_ROOT=/etc; }
VERBOSE=${VERBOSE:-}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-round.XXXXXX")
trap 'rm -rf "$TMP"; [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null' EXIT

hr() { printf '\n\033[1m%s\033[0m\n' "$*"; }
sub() { printf '  %s\n' "$*"; }

[ -x "$PROBE" ] || make etp-probe >/dev/null

# ---------------------------------------------------------------- 1. scan

hr "1. scan  ($TEST_ROOT)"
DB="$TMP/round.idx"
"$BIN" build "$TEST_ROOT" -o "$DB" 2>&1 >/dev/null | sed 's/^/  /'
ESIDX_LOG=info "$BIN" build "$TEST_ROOT" -o "$TMP/r2.idx" 2>&1 >/dev/null \
    | grep -E 'finalize:|scan:' | sed 's/^/  /'

# ---------------------------------------------------------------- 2. serve

hr "2. serve"
"$BIN" -v 3 serve "$DB" -p 0 --bind 127.0.0.1 >"$TMP/srv.out" 2>"$TMP/srv.err" &
PID=$!
for _ in $(seq 1 100); do
    PORT=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$TMP/srv.err" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    sleep 0.05
done
[ -n "${PORT:-}" ] || { echo "server did not start"; cat "$TMP/srv.err"; exit 1; }
sed -n '1,2p' "$TMP/srv.err" | sed 's/^/  /'

# drive <name> <script on stdin> -- prints the timing the server logged
#
# Each drive is a separate connection, so the login has to be part of every script.
# Injecting it here rather than in ten heredocs also means a step cannot silently
# be run unauthenticated -- which fails confusingly, because the server answers
# 530 to everything and the QUERY reply the probe expects is never a query block.
drive() {
    local name="$1"
    {
        echo "send USER anonymous"
        if [ "$name" != "connect" ]; then
            cat
        fi
    } >"$TMP/script"
    [ -n "$VERBOSE" ] && sed 's/^/    > /' "$TMP/script"
    local before
    before=$(wc -l <"$TMP/srv.err")
    timeout 30 "$PROBE" "$PORT" "$TMP/script" >"$TMP/probe.out" 2>&1
    printf '\n  \033[1m%s\033[0m\n' "$name"
    sed -nE 's/^ROW ([0-9]+) (FILE|FOLDER) (.*) path=([^ ]*) size=(-?[0-9]+).*/    \2  \3/p' \
        "$TMP/probe.out" | head -8
    sed -nE 's/^BLOCK-END [0-9]+ count=([0-9]+) rows=([0-9]+).*/    RESULT_COUNT \1, \2 row(s) on the wire/p' \
        "$TMP/probe.out"
    tail -n +$((before + 1)) "$TMP/srv.err" | grep -E 'query:|cache hit' | sed 's/^/    /'
    grep -E 'PROTOCOL-ERROR' "$TMP/probe.out" | sed 's/^/    !! /'
    return 0
}


TREE_WS=${TEST_ROOT//\//\\}

# ------------------------------------------------- 3. the session a client runs

hr "3. the session the ETP client performs"

drive "connect, then nothing else -- the handshake on its own" <<'EOF'
send NOOP
EOF

drive "browse the root: parent: + folder:, name_ascending, COUNT 200" <<EOF
send EVERYTHING CASE 0
send EVERYTHING PATH 0
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING ATTRIBUTES_COLUMN 1
send EVERYTHING DATE_MODIFIED_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING SORT name_ascending
send EVERYTHING OFFSET 0
send EVERYTHING COUNT 200
send EVERYTHING SEARCH parent:"$TEST_ROOT" folder:
sendraw EVERYTHING QUERY
query
EOF

drive "the same, files only -- the client issues this as a second round trip" <<EOF
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING ATTRIBUTES_COLUMN 1
send EVERYTHING DATE_MODIFIED_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING SORT name_ascending
send EVERYTHING OFFSET 0
send EVERYTHING COUNT 200
send EVERYTHING SEARCH parent:"$TEST_ROOT" !folder:
sendraw EVERYTHING QUERY
query
EOF

drive "drill down into a real subdirectory" <<EOF
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 200
send EVERYTHING SEARCH parent:"$TEST_ROOT/apache2" folder:
sendraw EVERYTHING QUERY
query
EOF

drive "page 1 of a search, COUNT 50" <<EOF
send EVERYTHING CASE 0
send EVERYTHING PATH 1
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING ATTRIBUTES_COLUMN 1
send EVERYTHING DATE_MODIFIED_COLUMN 1
send EVERYTHING DATE_CREATED_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING SORT date_modified_descending
send EVERYTHING OFFSET 0
send EVERYTHING COUNT 50
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
EOF

drive "two pages on ONE connection -- the second is a cache re-slice" <<'EOF'
send EVERYTHING CASE 0
send EVERYTHING PATH 1
send EVERYTHING SORT date_modified_descending
send EVERYTHING OFFSET 0
send EVERYTHING COUNT 50
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
send EVERYTHING OFFSET 50
sendraw EVERYTHING QUERY
query
EOF
_pages=$(sed -nE 's/^BLOCK-END ([0-9]) count=([0-9]+) rows=([0-9]+).*/\3/p' "$TMP/probe.out" | tr '\n' '/')
_counts=$(sed -nE 's/^BLOCK-END ([0-9]) count=([0-9]+) rows=([0-9]+).*/\2/p' "$TMP/probe.out" | tr '\n' '/')
sub "rows per page: $_pages   RESULT_COUNT per page: $_counts"
sub "(both pages report the same total, and only the first one ran a search)"

drive "a category query, the client's quick-search default" <<EOF
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 50
send EVERYTHING SEARCH image:
sendraw EVERYTHING QUERY
query
EOF

drive "a path search with a wildcard, sorted by size" <<EOF
send EVERYTHING SORT size_descending
send EVERYTHING COUNT 50
send EVERYTHING SEARCH path:$TEST_ROOT *.conf size:>1k
sendraw EVERYTHING QUERY
query
EOF

drive "a second-stage filter on top of the primary search" <<EOF
send EVERYTHING COUNT 100
send EVERYTHING SEARCH ext:conf
send EVERYTHING FILTER_SEARCH ca
sendraw EVERYTHING QUERY
query
EOF

drive "COUNT omitted entirely -- the reference default is 'unlimited'" <<'EOF'
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
EOF

drive "disconnect" <<'EOF'
sendraw QUIT
close
EOF

# ------------------------------------------------------------- 4. the numbers

hr "4. where the time goes (from the server log above)"
sub "candidate seeding is visible per query as 'seeded N of M candidates'"
sub "a browse request seeds from parent:, so the matcher pass walks a directory"
sub "listing rather than the index; an unfiltered query has no such anchor"
echo
grep -oE 'seeded [0-9]+ of [0-9]+ candidates' "$TMP/srv.err" | sort -u | sed 's/^/  /'
echo
sub "cache hits:"
grep -c 'cache hit' "$TMP/srv.err" | sed 's/^/  /'

hr "round complete"
