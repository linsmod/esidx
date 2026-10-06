#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
# What skipping a derived index costs and what it saves, on one tree, with one binary.
#
# The two configurations differ only in ESIDX_SKIP_INDEX, and the *same snapshot* serves
# both -- which is what makes this measurable at all (D4: the derived indexes are not in
# the file, so a snapshot written by a process that built everything answers queries for a
# process that built less). Alternating the two and taking the best of N, because one run
# on a shared box is a rumour.
#
# It prints the matched row count from both configurations on every line, because the
# claim being measured is that they agree: a degraded path that is faster *and* returns
# something else is not a trade, it is a bug with a number attached.
#
#   ./tri-skip.sh [tree] [N]
set -u
ROOT=${1:-/usr}
N=${2:-7}
BIN=${ESIDX_BIN:-./esidx}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-skip.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

[ -x "$BIN" ] || { echo "no binary at $BIN -- run make, or set ESIDX_BIN"; exit 1; }
[ -d "$ROOT" ] || { echo "no such tree: $ROOT"; exit 1; }

"$BIN" build "$ROOT" -o "$TMP/a.idx" >/dev/null 2>&1 || { echo "build failed"; exit 1; }

# one <skip> <query...> -> "<rows> <plan> <eval> <sort>"
one() {
    local skip="$1"; shift
    ESIDX_SKIP_INDEX="$skip" ESIDX_LOG=info "$BIN" query "$TMP/a.idx" "$@" 2>&1 >/dev/null \
        | sed -nE 's/.*: ([0-9]+) matched .*plan ([0-9.]+) ms, eval ([0-9.]+) ms, sort ([0-9.]+) ms.*/\1 \2 \3 \4/p' \
        | tail -1
}

# mins <file> -> "rows plan eval sort", each the minimum over that file's runs. Per
# field: sorting a sample's four numbers against each other would report the best plan
# and the worst sort as if they came from one run. The sort column is not optional --
# dropping the name rank costs nothing in eval and everything in sort.
mins() {
    local f=1 out=""
    while [ "$f" -le 4 ]; do
        out="$out $(sort -k"$f","$f"n "$1" | head -1 | cut -d' ' -f"$f")"
        f=$((f + 1))
    done
    printf '%s' "${out# }"
}

SKIP=""
SKIPQ=""
hdr() {
    printf '\n%s, skipped: %s   (best of %s, ms)\n' "$ROOT" "$SKIP" "$N"
    printf '%-32s %8s %8s %8s %8s  | %8s %8s %8s %8s\n' \
           "query" "rows" "plan" "eval" "sort" "rows" "plan" "eval" "sort"
    printf -- '------------------------------------------------------------------------------\n'
}

best() {
    local on="$TMP/on" off="$TMP/off" i
    : >"$on"; : >"$off"
    for i in $(seq 1 "$N"); do
        one ""     "$@" >>"$on"
        one "$SKIP" "$@" >>"$off"
    done
    local a b
    a=$(mins "$on"); b=$(mins "$off")
    # shellcheck disable=SC2086
    printf '%-32s %8s %8s %8s %8s  | %8s %8s %8s %8s  %s\n' \
        "$SKIPQ" $a $b \
        "$( [ "${a%% *}" = "${b%% *}" ] && echo same || echo DIFFERENT )"
}

SKIP=size;       hdr; SKIPQ='size:>1k';                   best "size:>1k" "count:50"
SKIPQ='size:1000..50000';           best "size:1000..50000" "count:50"
SKIPQ='size:>1k file:';             best "size:>1k" "file:" "count:50"
# The three above match most of /usr, and a range that selects almost nothing measures
# the wrong thing twice over: eval is dominated by the sort either way, and the sort is
# over the whole table rather than over what the range selected. These two are the
# shapes the array exists for, and they are the ones that decide whether it earns
# 12.8 MiB of resident index.
SKIPQ='size:<1k';                   best "size:<1k" "count:50"
SKIPQ='size:>10mb';                 best "size:>10mb" "count:50"

SKIP=mtime;      hdr; SKIPQ='dm:today';                   best "dm:today" "count:50"

SKIP=ctime;      hdr; SKIPQ='dc:>2000';                   best "dc:>2000" "count:50"

SKIP=size,mtime,ctime
                 hdr; SKIPQ='size:>1k';                   best "size:>1k" "count:50"
SKIPQ='dm:today';                   best "dm:today" "count:50"

SKIP=trigram;    hdr; SKIPQ='conf';                       best "conf" "count:50"
SKIPQ="path:$ROOT *.conf size:>1k"; best "path:$ROOT" "*.conf" "size:>1k" "count:50"

SKIP=rank;       hdr; SKIPQ='image: sort:name:asc';       best "image:" "sort:name:asc" "count:50"

printf '\nDIFFERENT in the last column means the degraded path answered something else,\n'
printf 'which is the failure this harness exists to make visible.\n'
