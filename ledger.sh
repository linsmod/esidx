#!/usr/bin/env bash
#
# The memory ledger and the phase timings for one tree, on one build.
#
#   ./ledger.sh                # /usr -- the tree the query tables are measured on
#   ./ledger.sh /work          # 5.5 M entries -- the tree the memory work is measured on
#   ESIDX_BIN=/path/to/esidx ./ledger.sh /work
#
# This is the script every memory number in README.md and design §10 comes from, and
# it is in the repo because a number whose command is not is folklore (AGENTS.md 1.3).
# The ledger rows themselves come from esidx_log_mem(), but *which* build printed
# them, on which tree, with which snapshot, and which query shapes ran against it is
# not something the log says -- which is how a /work figure ends up quoted in a commit
# message with nothing left to re-run it from.
#
# Not a test: nothing is asserted here, and test.sh and test_etp.sh are. The value is
# that two runs of this on the same tree and the same machine are comparable, so a
# before and an after can be taken with one command each.

set -u
cd "$(dirname "$0")"

BIN=${ESIDX_BIN:-./esidx}
TREE=${1:-/usr}
DB=${ESIDX_LEDGER_DB:-/tmp/esidx-ledger.idx}

[ -x "$BIN" ] || { echo "no binary at $BIN -- run make, or set ESIDX_BIN"; exit 1; }
[ -d "$TREE" ] || { echo "no such tree: $TREE"; exit 1; }

hr() { printf '\n\033[1m%s\033[0m\n' "$*"; }

# ------------------------------------------------------------------ 1. the build
#
# -v 3 is LOG_INFO, which is where esidx_log_stats() and esidx_log_mem() print
# (AGENTS.md 2.3: the wire trace is -v 4, and the per-syscall walk split is -v 5 --
# neither of which belongs in a memory measurement, because both make the build do
# work it would not otherwise do).

hr "1. build  ($TREE)"
BUILD_LOG=$(mktemp "${TMPDIR:-/tmp}/esidx-ledger.XXXXXX")
trap 'rm -f "$BUILD_LOG"' EXIT
"$BIN" -v 3 build "$TREE" -o "$DB" >/dev/null 2>"$BUILD_LOG"

grep -E 'mem |peak rss|entries=|ext intern|indexed ' "$BUILD_LOG" | sed 's/^/  /'

hr "2. snapshot"
ls -l "$DB" | awk '{printf "  %s bytes\n", $5}'

# ------------------------------------------------------------- 3. the query shapes
#
# The four shapes README.md quotes, plus the two the storage work touches directly.
# Each is run once, on a freshly loaded index, so the numbers are a cold read of the
# structures the ledger above just described -- which is the point: a memory change
# that moved a column out of a hot loop shows up here and nowhere else.

hr "3. query shapes  (one run each, candidate count first)"
while IFS='|' read -r label expr; do
    [ -n "$label" ] || continue
    line=$("$BIN" -v 3 query "$DB" "$expr" 2>&1 >/dev/null \
           | sed -nE 's/.*query: ([0-9]+) matched.*\| plan ([0-9.]+) ms, eval ([0-9.]+) ms, sort ([0-9.]+) ms.*seeded ([0-9]+) of ([0-9]+) candidates.*/\1 \2 \3 \4 \5 \6/p')
    [ -n "$line" ] || { printf '  %-16s (no query line -- parse error?)\n' "$label"; continue; }
    set -- $line
    printf '  %-16s matched=%-8s plan/eval/sort ms=%s/%s/%s  candidates=%s of %s\n' \
           "$label" "$1" "$2" "$3" "$4" "$5" "$6"
done <<EOF
browse|parent:"$TREE" folder:
ext:conf|ext:conf
bare word|conf
wildcard+size|path:$TREE *.conf size:>1k
category|image:
EOF

hr "ledger complete"
