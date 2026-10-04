#!/usr/bin/env bash
# sortcmp.sh -- per-sort-key cost, for as many builds of esidx as you hand it.
#
# Why this exists: `round.sh` measures a *session*, and every sort key in it is a
# filtered result set or a browse. That left the sort cost unattributed, and it turned
# out to be the largest single line item on a real tree -- so the numbers that decided
# this layer were produced by a throwaway script, which is exactly the state AGENTS.md
# 1.3 says not to leave a repository in.
#
# Usage:
#   ./sortcmp.sh [-n REPEATS] [-t TREE] LABEL=BINARY [LABEL=BINARY ...]
#
#   -n REPEATS   runs per cell, minimum reported (default 3). A single run on a shared
#                box is a rumour, and the spread across repeats is larger than most of
#                the differences being argued about.
#   -t TREE      tree to index (default /usr).
#
#   Each LABEL=BINARY is a path to an esidx. They are indexed onto the same tree,
#   interleaved per key, and every row is one unfiltered sort over the whole index --
#   which is the shape that makes the sort dominate.
#
# Recipe for the before/after comparison this is for:
#
#   git archive <baseline-commit> | tar -x -C /tmp/base && (cd /tmp/base && make)
#   ./sortcmp.sh base=/tmp/base/esidx mine=./esidx
#
# The labels are yours; the script does not assume which one is the baseline, except
# that the first one is the one every gain is computed against.
#
# What it reports, and why each column is here:
#   sort ms      the sort phase, from main.c's INFO line. That line is the one shape
#                every build shares -- query.c's DEBUG line carries the key/comparison
#                breakdown and did not exist at all at the baseline commit, which is
#                what silently emptied the baseline column when this first ran.
#   comparisons  from query.c's DEBUG line, so ns-per-comparison is derivable rather
#                than guessed. Without it "the comparator is faster" and "there were
#                fewer comparisons" look identical, and they are different fixes.
#   finalize ms  what a build pays to *have* the index. A change that makes a query
#                2x faster and a build 20 % slower is a trade, not a win, and the
#                trade belongs in the same table as the gain.

set -u

REPEATS=3
TREE=/usr
BINS=""
while [ $# -gt 0 ]; do
    case "$1" in
        -n) REPEATS="$2"; shift 2 ;;
        -t) TREE="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *=*) BINS="$BINS $1"; shift ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[ -n "$BINS" ] || { printf 'usage: %s [-n N] [-t TREE] LABEL=BINARY ...\n' "$0" >&2; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-sortcmp.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# labels and binaries, parallel lists
LABELS=""; PATHS=""
for pair in $BINS; do
    LABELS="$LABELS ${pair%%=*}"
    PATHS="$PATHS ${pair#*=}"
done
set -- $LABELS; LABELS="$*"
set -- $PATHS; PATHS="$*"
NSORT=0
for pair in $BINS; do NSORT=$((NSORT + 1)); done
# Captured before anything else calls `set --`: the header below needs the first
# label, and $1 is not it by then.
BASE_LABEL=${LABELS%% *}

KEYS="name:ascending name:descending path:ascending path:descending \
      extension:ascending date_modified:descending size:descending \
      attributes:ascending date_created:descending"

for i in $(seq 1 "$NSORT"); do
    eval "label_$i=\$(echo \$LABELS | cut -d' ' -f$i)"
    eval "bin_$i=\$(echo \$PATHS  | cut -d' ' -f$i)"
    [ -x "$(eval echo \$bin_$i)" ] || { printf 'not executable: %s\n' "$(eval echo \$bin_$i)" >&2; exit 2; }
    eval "db_$i=\"\$TMP/\$(eval echo \\\$label_$i).idx\""
    "$(eval echo \$bin_$i)" build "$TREE" -o "$(eval echo \$db_$i)" 2>/dev/null \
        || { printf 'build failed: %s\n' "$(eval echo \$label_$i)" >&2; exit 2; }
done
printf 'indexed %s: ' "$TREE"
for i in $(seq 1 "$NSORT"); do
    eval "r=\$(\$(eval echo \\\$bin_$i) query \"\$(eval echo \\\$db_$i)\" '' 'count:0' 2>/dev/null | grep -c .)"
    printf '%s=%s ' "$(eval echo \$label_$i)" "$r"
done
printf '\n'

# one <bin> <db> <key> -> the sort phase in ms
one() {
    ESIDX_LOG=info timeout 600 "$1" query "$2" "sort:$3" 'count:50' 2>&1 >/dev/null \
        | grep -oE 'sort [0-9.]+ ms' | tail -1 | awk '{print $2}'
}
# ncmp <bin> <db> <key> -> comparisons performed, or nothing if the build predates it
ncmp() {
    ESIDX_LOG=debug timeout 600 "$1" query "$2" "sort:$3" 'count:50' 2>&1 >/dev/null \
        | grep -oE '[0-9]+ comparisons' | tail -1 | awk '{print $1}'
}
# best <bin> <db> <key> -> the minimum over REPEATS
best() {
    _s=$(one "$1" "$2" "$3"); _i=1
    while [ "$_i" -lt "$REPEATS" ]; do
        _v=$(one "$1" "$2" "$3")
        _s=$(printf '%s\n%s\n' "$_s" "$_v" | grep -E '^[0-9.]+$' | sort -g | head -1)
        _i=$((_i + 1))
    done
    printf '%s\n' "${_s:-?}"
}

printf '\nunfiltered over %s, best of %s, sort phase in ms\n' "$TREE" "$REPEATS"
printf '%-24s' "sort key"
for i in $(seq 1 "$NSORT"); do eval "lb=\$(eval echo \\\$label_$i)"; printf '%10s' "$lb"; done
for i in $(seq 2 "$NSORT"); do
    eval "lb=\$(eval echo \\\$label_$i)"
    printf ' %8s' "gain/$lb"
done
printf ' %12s\n' comparisons
printf -- '--------------------------------------------------------------------------\n'

for k in $KEYS; do
    printf '%-24s' "$k"
    vals=""
    for i in $(seq 1 "$NSORT"); do
        eval "v=\$(best \"\$(eval echo \\\$bin_$i)\" \"\$(eval echo \\\$db_$i)\" $k)"
        eval "cell_$i=\$v"
        printf '%10s' "$v"
        vals="$vals $v"
    done
    for i in $(seq 2 "$NSORT"); do
        eval "b=\$cell_1; c=\$cell_$i"
        printf ' %7sx' "$(awk -v x="$b" -v y="$c" 'BEGIN{printf "%.2f", (y+0>0)?x/y:0}')"
    done
    eval "last=$NSORT"
    printf ' %12s\n' "$(ncmp "$(eval echo \$bin_$last)" "$(eval echo \$db_$last)" "$k")"
done

echo
echo "--- what each build pays to have the index"
echo "   (finalize runs on build and again on load; the per-index lines are the cost of"
echo "    each derived index, and load is the whole of it end to end)"
for i in $(seq 1 "$NSORT"); do
    eval "bn=\$(eval echo \\\$bin_$i); lb=\$(eval echo \\\$label_$i)"
    # Build first, then load *that* snapshot: querying before the build leaves the
    # first row measuring nothing and every later row measuring the build before it.
    ESIDX_LOG=info "$bn" build "$TREE" -o "$TMP/probe.idx" >/dev/null 2>"$TMP/b.log"
    printf '  %-8s load ' "$lb"
    ESIDX_LOG=info "$bn" query "$TMP/probe.idx" '' 'count:1' 2>&1 >/dev/null \
        | sed -nE 's/.*load: total: ([0-9.]+) ms/\1 ms/p' | tr -d '\n'
    printf '  | '
    sed -nE 's/.*(finalize: (name rank|name trigrams|ext sets|type bitmaps)[^|]*)/\1/p' \
        "$TMP/b.log" | sed 's/^/ /' | tr '\n' ' '
    printf '\n'
done