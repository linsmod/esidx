#!/usr/bin/env bash
#
# Measure the in-place refresh path (design §7, `esidx serve --refresh=SECS`).
#
#   ./refresh.sh <root>              idle passes and the snapshot write, read-only
#   ./refresh.sh <root> --add N      also add N files to a scratch directory under
#                                     <root>, measure the pass that picks them up,
#                                     then remove it
#   REFRESH=SECS ./refresh.sh <root> the interval to serve with (default 2)
#
# Why this is a script in the repo and not a one-off (AGENTS.md 1.3): the numbers it
# prints are the ones quoted for the refresh path, and a number no reader can re-run is
# worth nothing. `sortcmp.sh` and `tri-skip.sh` are here for the same reason.
#
# The three costs it separates, because they differ by three orders of magnitude and only
# one of them is proportional to the change:
#
#   idle pass      the walk: one getdents per directory whose stamp moved, plus one stat
#                  per directory it descends into. Proportional to the *root's fanout*,
#                  not to the tree -- which is why an idle pass over 5.5 M entries costs
#                  about a millisecond.
#   adding pass    the same walk, plus -- and this is the number that decides whether an
#                  interval is usable at all -- one O(n log n) name-rank rebuild and one
#                  O(n) children-array rebuild, because neither can be appended to in
#                  place (esidx.h:114, design §5.1).
#   snapshot write the whole file, because no derived structure is persisted (D4).
#
# --add writes into the tree it is pointed at, and says so before it does.

set -u
cd "$(dirname "$0")"

BIN=${ESIDX_BIN:-./esidx}
PROBE=./etp-probe
SECS=${REFRESH:-2}
ROOT=""
ADD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --add)   ADD=${2:-2000}; shift 2 ;;
        --add=*) ADD=${1#--add=}; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        -*)      echo "unknown option $1"; exit 2 ;;
        *)  if [ -z "$ROOT" ]; then ROOT=$1; else
                echo "unexpected argument $1"; exit 2
            fi
            shift ;;
    esac
done
[ -n "$ROOT" ] || { echo "usage: ./refresh.sh <root> [--add N]"; exit 2; }
case "$ADD" in ''|*[!0-9]*) echo "--add needs a count"; exit 2 ;; esac

TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-refresh.XXXXXX")
PID=""
CHURN=0
cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    [ "$CHURN" = 1 ] && rm -rf "$ROOT/.esidx-refresh-churn"
    rm -rf "$TMP"
    return 0
}
trap cleanup EXIT
hr() { printf '\n\033[1m%s\033[0m\n' "$*"; }

[ -x "$PROBE" ] || make etp-probe >/dev/null

DB="$TMP/refresh.idx"
hr "index $ROOT"
"$BIN" build "$ROOT" -o "$DB" 2>&1 >/dev/null | grep -E '^indexed' | sed 's/^/  /'

hr "serve --refresh=$SECS"
"$BIN" -v 4 serve "$DB" -p 0 --bind 127.0.0.1 "--refresh=$SECS" \
    >"$TMP/srv.out" 2>"$TMP/srv.err" &
PID=$!
# The port is printed only after the load, and the load is finalize over the whole tree
# (7.5 s on /work), so poll generously instead of sleeping a fixed guess.
PORT=""
for _ in $(seq 1 600); do
    PORT=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$TMP/srv.err" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    sleep 0.5
done
[ -n "$PORT" ] || { echo "server did not start"; cat "$TMP/srv.err"; exit 1; }
sed -n '1,3p' "$TMP/srv.err" | sed 's/^/  /'
if [ "$ADD" -gt 0 ]; then
    printf '  \033[33mcreating %s files in %s/.esidx-refresh-churn, and removing them again\033[0m\n' \
        "$ADD" "$ROOT"
    [ -w "$ROOT" ] || { echo "$ROOT is not writable; --add needs a writable tree"; exit 1; }
fi

# query <search>: one query through the probe, and the server's own timing line
query() {
    cat >"$TMP/script" <<EOF
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 50
send EVERYTHING SEARCH $1
sendraw EVERYTHING QUERY
query
EOF
    if ! timeout 30 "$PROBE" "$PORT" "$TMP/script" >"$TMP/probe.out" 2>&1; then
        echo "  probe failed on '$1'"; grep -m3 PROTOCOL-ERROR "$TMP/probe.out"; return 1
    fi
    grep -F "query: '$1'" "$TMP/srv.err" | tail -1 | sed 's/^/  /'
}

passes() { grep -E 'update: [0-9]+ dirs' "$TMP/srv.err" | tail -"$1" | sed 's/^/  /'; }

hr "startup repair pass, then idle passes"
sleep $((SECS * 3))
grep 'startup repair pass' "$TMP/srv.err" | sed 's/^/  /'
passes 3

# One range query, before anything is added. esidx_add pushes each new row into the three
# numeric deltas (D3), so this line and the one after the churn are the measurement of
# what that costs a range query -- the delta is walked in append order on every one
# (store.c:1334), so it is linear in the delta and this is the only place it shows up.
# The search is deliberately a *selective* range: `size:>1k` on /work matches four
# million rows, and what that measures is the sort, not the delta.
hr "a range query, delta empty"
query 'size:>1mb'

if [ "$ADD" -gt 0 ]; then
    hr "add $ADD files, and the pass that finds them"
    CHURN=1
    mkdir -p "$ROOT/.esidx-refresh-churn"
    # One file per iteration, and *not* a stride: a loop that steps i by more than 1
    # creates fewer files than it says, which is how a first version of this script
    # reported "add 2000 files" while adding two -- and the totals it then printed (the
    # two O(n) rebuilds) were identical either way, because they do not depend on how
    # many rows were added. The row count is what the delta measurement needs to be real.
    i=0
    while [ "$i" -lt "$ADD" ]; do
        printf 'x%.0s' $(seq 1 100) >"$ROOT/.esidx-refresh-churn/f$i.dat"
        i=$((i + 1))
    done
    ls "$ROOT/.esidx-refresh-churn" | wc -l | sed 's/^/  files created: /'
    sleep $((SECS * 2))
    passes 2
    grep -E 'name rank rebuilt|children array rebuilt' "$TMP/srv.err" | tail -2 | sed 's/^/  /'

    hr "the same range query, with those rows in the delta"
    query 'size:>1mb'

    hr "remove them again"
    rm -rf "$ROOT/.esidx-refresh-churn"
    CHURN=0
    sleep $((SECS * 2))
    passes 2
fi

hr "snapshot write: the clean-exit save"
query 'ext:conf'
kill "$PID"; wait "$PID" 2>/dev/null; PID=
if grep -E 'save:|refresh: wrote' "$TMP/srv.err" >"$TMP/save.txt"; then
    sed 's/^/  /' "$TMP/save.txt"
else
    # The common case, and the one worth seeing printed: the epoch never moved, so the
    # process had nothing to write. A server rewriting an unchanged snapshot on a timer
    # would be paying a full-file write to say nothing.
    echo "  nothing was written: the index did not change, so the epoch never moved"
fi
ls -la "$DB" 2>/dev/null | sed 's/^/  /'

hr "refresh complete"
