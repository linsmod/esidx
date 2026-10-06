#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
#
# Compare this server against voidtools' own, on one directory both of them index.
#
#   ./cmp_ref.sh                  the pinned term list, on the parent of this repo
#   ./cmp_ref.sh /mnt/c/Users/x   a different directory the reference can also see
#   ./cmp_ref.sh /mnt/c/Users/x 'foo' 'bar'   just these terms
#   ./cmp_ref.sh --strict         exit 1 on a row the index delta does not explain
#   ESIDX_CMP_SAMPLE=0            do not print the entries the delta is made of
#
# This is not a test -- test.sh and test_etp.sh are, and they are pinned against
# find(1) and the wire. This is the other half of AGENTS.md 1.4: the place a
# *number* in a comment, in a commit message or in docs/design.md 12.10 comes from.
# Every expected value quoted there was measured here, against the reference server
# on 127.0.0.1:21, with the only difference between the two queries being the
# spelling of the directory -- this server is handed the POSIX one and the
# reference the Windows one -- so a pattern cannot slip through a separator and
# the two replies have to agree.
#
# Requirements: a reference (etp_server) listening on :21, and this build.
# Both are on this machine already; `make etp-probe` builds the probe.
#
# Reading the output. One row per term, ref against ours. The two `tree:` rows come
# first and state how far apart the two *indexes* are before anything else, because
# that difference is what every later row is measured against: a row that differs by
# exactly it is marked `= delta`, anything else is marked DIFF -- which is how the
# term-scope rules in design 12.10 were pinned rather than guessed. `--strict` fails
# on a DIFF row, i.e. on any disagreement the index delta does not already explain.
#
# The delta is a difference between two *databases*, and it has two causes that mean
# opposite things, so the script prints the entries that make it up rather than
# leaving the reader to assume a benign one (ESIDX_CMP_SAMPLE of each kind, 0 to
# skip; ESIDX_CMP_MAX_DUMP caps the dump). Each is marked with whether the file is
# on disk right now:
#
#   not on disk   a leftover in the reference's index. Everything on :21 is a
#                 live NTFS index and it lags: git's `tmp_obj_XXXXXX` loose-object
#                 temp files, renamed away long ago, were still in it when this
#                 was written. Our smaller number is the complete one.
#   ON DISK       we failed to index a file that exists. That is ours to fix, and
#                 no term row means anything until it is.
#
# The first cause was originally written up here as "the reference indexes files
# /mnt/c cannot see", which is a different thing and was never measured. AGENTS.md
# 1.3 says a quoted number is only worth something if the command that produced it
# is here; that is now true of the delta's *reason* as well as its size.

set -u
cd "$(dirname "$0")"

BIN=./esidx
PROBE=./etp-probe
REF_PORT=${ESIDX_REF_PORT:-21}
OUR_PORT=${ESIDX_CMP_PORT:-2199}
# rows of each kind to print when the two indexes disagree, and the size past which
# the dump that explains it is more expensive than the explanation is worth
SAMPLE=${ESIDX_CMP_SAMPLE:-10}
MAX_DUMP=${ESIDX_CMP_MAX_DUMP:-200000}

STRICT=0
[ "${1:-}" = "--strict" ] && { STRICT=1; shift; }
DIR=${1:-$(dirname "$(pwd)")}
[ $# -gt 0 ] && shift

# The same directory as the reference spells it. wslpath is the only thing here
# that knows how a Linux path maps onto a Windows one, and getting that wrong is
# what made the first version of this script measure nothing at all.
WIN_DIR=$(wslpath -w "$DIR" 2>/dev/null)
case $WIN_DIR in
    [A-Za-z]:*) ;;
    *) printf 'cannot map %s onto a Windows path.\n' "$DIR"
       printf 'The reference indexes the Windows filesystem, so the directory has to\n'
       printf 'be one it can see -- that means under a mounted drive.  The parent of\n'
       printf 'this repo is the default because both servers can reach it; /usr is not.\n'
       exit 2 ;;
esac

# The two servers spell the same file differently and nothing else: the reference
# says `C:\Users\x`, we say `/mnt/c/Users/x`. explain_delta() strips one prefix from
# each side so the two sets can be compared as the paths they are.
DRIVE_PREFIX="${WIN_DIR%%:*}:"
MOUNT="/mnt/$(printf '%s' "${WIN_DIR%%:*}" | tr 'A-Z' 'a-z')"

# The terms docs/design.md 12.10 and test.sh cite, kept in one list so every number
# in those places can be re-measured with one command. A term quoted there belongs
# here, or the next reader cannot check it.
TERMS=(
    # no separator in the value: the filename, never the path
    'esidx' 'name:esidx' '*esidx*' '*esidx' 'regex:esidx' 'ww:esidx' 'whole:esidx'
    'regex:ShareToPC.esidx'
    # path:, or a separator in the value: the path
    'path:esidx' 'esidx/main.c' 'esidx\main.c' 'sidx/main.c'
    # a wildcard over a path is anchored at a component boundary
    'esidx/*' 'esidx\*' 'sidx/*' '*esidx/main.c'
    'folder: esidx/*' 'path:esidx/main*' 'path:*/main.c' 'path:*esidx/main.c'
    # path: and a leading star is contains -- the one shape where a star crosses
    'path:*esidx*' 'path:**esidx**' 'path:*PC*' 'path:*esidx' 'path:*PC/esidx*'
    'path:*esidx/store.c' 'path:*esidx/deep' 'path:*esidxx*'
    # a bare term with a separator is not contains, however it is spelled
    '*PC/esidx*'
    # the rest of the matcher, which the reference and we already agreed on
    'stem:main' 'startwith:sub' 'ext:c esidx' 'folder: esidx' 'nope-does-not-exist'
    # a prefix name followed directly by a term, no space: `folder:abc` is the folder
    # filter AND the term `abc` (docs/everything-syntax.md, "what this does not settle").
    # A nonsense term is the whole test: the reference answers 0 where `folder:` alone
    # answers 1 594 989, so a parser that dropped the value would answer the bare filter
    # here and this row would say so. `empty:` and a macro are in for the same reason --
    # every name that can stand alone takes a term, measured on :21.
    'folder:zzzznotfound' 'image:zzzznotfound' 'empty:zzzznotfound'
)
[ $# -gt 0 ] && TERMS=("$@")

# One term derived from DIR rather than pinned, because the shape it covers is the
# indexed root: an entry's name is the last component of its own path, so `name:` on
# the directory *above* the root has to match nothing -- the root's path mentions it
# and no entry is named after it. We got this wrong (ref 0, ours 1) and no term in
# the list read the root row, so it is a term now. Skipped when the name is not a
# single word, because a space would split the search line and measure something else.
ROOT_PARENT=$(basename "$(dirname "$DIR")")
case $ROOT_PARENT in
    "" | *[![:alnum:]]*) ;;
    *) TERMS+=("name:$ROOT_PARENT") ;;
esac

# A term longer than every fixed buffer this server used to have on the way in, with
# the extension that exists *last* so that a cut anywhere drops it and the answer
# collapses. It is a term because three of them were found by asking this question of
# the reference and not of ourselves: the search buffer (4096), the acknowledgement
# c_reply() formatted (1023, which hung the client rather than answering wrongly), and
# the parser's per-term value (2048). All three now hold a value this long, and the
# reference always did -- it reallocs the search per SEARCH (etp_server.c:4025) and
# prints the acknowledgement straight into the client's stream (etp_server.c:4027).
PAD=$(for i in $(seq 1 400); do printf 'zzzzzzzzzzzzzz%s;' "$i"; done)
TERMS+=("ext:${PAD}c")

TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-cmp.XXXXXX")
trap 'rm -rf "$TMP"; pkill -x esidx 2>/dev/null' EXIT

[ -x "$PROBE" ] || make etp-probe >/dev/null

printf 'directory  ours %s\n' "$DIR"
printf '           ref  %s\n\n' "$WIN_DIR"

"$BIN" build "$DIR" -o "$TMP/db.idx" 2>&1 >/dev/null | sed 's/^/  /'

# setsid nohup, or the server dies with this shell (AGENTS.md 1.4).
setsid nohup "$BIN" -v 3 serve "$TMP/db.idx" -p "$OUR_PORT" --bind 127.0.0.1 \
    >"$TMP/srv.log" 2>&1 </dev/null &
sleep 1
grep -q 'esidx serving' "$TMP/srv.log" || {
    printf 'our server did not start:\n'; cat "$TMP/srv.log"; exit 2; }

# script <count> <search> [setup line ...] -> path of a probe script in $TMP
# The one place that knows what a probe script looks like, so rows() and dump_paths()
# cannot drift apart on the rules the probe applies (AGENTS.md 3.3).
script() {
    local n=$1 s=$2
    shift 2
    {
        echo 'send USER anonymous'
        printf '%s\n' "$@"
        echo 'send EVERYTHING COUNT '"$n"
        printf 'send EVERYTHING SEARCH %s\n' "$s"
        echo 'sendraw EVERYTHING QUERY'
        echo 'query'
        echo 'close'
    } >"$TMP/script"
    printf '%s' "$TMP/script"
}

# rows <port> <search> -> the row count the probe read back, empty if it got none
rows() {
    "$PROBE" "$1" "$(script 20000 "$2")" 2>/dev/null >"$TMP/raw"
    sed -n 's/.*count=\([0-9]*\).*/\1/p' "$TMP/raw" | head -1
}

# dump_paths <port> <search> <count> <strip-prefix> -> the whole result set as full
# paths, sorted, with the separators and the drive-or-mount prefix normalised away so
# the two servers' sets can be comm'd. The PATH column is the *parent* directory
# (etp.c:235), so the name has to come off the FILE/FOLDER field; the probe's per-row
# summary line carries both, which is why this reads its output rather than the wire.
dump_paths() {
    "$PROBE" "$1" "$(script "$3" "$2" 'send SITE EVERYTHING PATH_COLUMN 1')" 2>/dev/null \
        | sed -E 's/^ROW [0-9]+ (FILE|FOLDER) //' \
        | awk '{
              i = index($0, " path="); j = index($0, " size=");
              if (i == 0 || j <= i) next;
              print substr($0, i + 6, j - i - 6) "\\" substr($0, 1, i - 1);
           }' \
        | sed -e 's#\\#/#g' -e "s#^$4##" \
        | sort
}

# explain_delta -- print the entries the two indexes disagree on, and say which side
# is wrong. The verdict is a stat(2) of the path on *our* side, because that is the
# only one of the two that can be checked from this shell: the reference is a live
# NTFS index and there is no way to ask it what is still on disk.
explain_delta() {
    local cap=$(( (REF_TREE > OUR_TREE ? REF_TREE : OUR_TREE) + 16 ))
    local shown p verdict missing=0
    if [ "$SAMPLE" -le 0 ]; then
        printf '\ndelta %s: not sampled (ESIDX_CMP_SAMPLE=0)\n' "$DELTA"
        return
    fi
    if [ "$cap" -gt "$MAX_DUMP" ]; then
        printf '\ndelta %s: not sampled, %s rows is past the %s dump cap\n' \
            "$DELTA" "$cap" "$MAX_DUMP"
        return
    fi
    dump_paths "$REF_PORT"  "path:$WIN_DIR" "$cap" "$DRIVE_PREFIX" >"$TMP/ref.set"
    dump_paths "$OUR_PORT" "path:$DIR"      "$cap" "$MOUNT"           >"$TMP/our.set"
    comm -23 "$TMP/ref.set" "$TMP/our.set" >"$TMP/only_ref"
    comm -13 "$TMP/ref.set" "$TMP/our.set" >"$TMP/only_our"
    printf '\ndelta %s: %s row(s) only the reference has, %s only we have\n' \
        "$DELTA" "$(wc -l <"$TMP/only_ref")" "$(wc -l <"$TMP/only_our")"
    # the loops redirect from a file, not a pipe: a pipeline would put them in a
    # subshell and the `missing` tally would not survive back here
    for side in ref our; do
        shown=$(wc -l <"$TMP/only_$side")
        while read -r p; do
            [ -n "$p" ] || continue
            if [ -e "$MOUNT$p" ]; then
                verdict='ON DISK'
                [ "$side" = ref ] && missing=$((missing + 1))
            else
                verdict='not on disk'
            fi
            printf '  %-9s %-68s %s\n' "$side-only" "$p" "$verdict"
        done <"$TMP/only_$side"
        [ "$shown" -gt "$SAMPLE" ] &&
            printf '  %-9s ... and %s more\n' "$side-only" "$((shown - SAMPLE))"
    done
    if [ "$missing" -gt 0 ]; then
        printf '  => %s of them are on disk and we did not index them: ours is wrong.\n' \
            "$missing"
    else
        printf '  => none of them are on disk: leftovers in the reference index, so\n'
        printf '     our smaller number is the complete one.\n'
    fi
}

# one <label> <ref-search> <our-search>
one() {
    local r o d
    r=$(rows "$REF_PORT" "$2")
    o=$(rows "$OUR_PORT" "$3")
    if [ -z "$r" ] || [ -z "$o" ]; then
        printf '%-26s %-9s %-9s %s\n' "$1" "${r:-refused}" "${o:-refused}" 'DIFF'
        DIFFS=$((DIFFS + 1)); return
    fi
    d=$((r - o))
    if [ "$d" = 0 ]; then
        printf '%-26s %-9s %-9s %s\n' "$1" "$r" "$o" '='
    elif [ "$d" = "$DELTA" ]; then
        printf '%-26s %-9s %-9s %s\n' "$1" "$r" "$o" "= delta $d"
    else
        printf '%-26s %-9s %-9s %s\n' "$1" "$r" "$o" "DIFF $d"
        DIFFS=$((DIFFS + 1))
    fi
}

REF_TREE=$(rows "$REF_PORT" "path:$WIN_DIR")
OUR_TREE=$(rows "$OUR_PORT" "path:$DIR")
DELTA=$((${REF_TREE:-0} - ${OUR_TREE:-0}))

printf '%-26s %-9s %-9s %s\n' TERM REF OURS ''
DIFFS=0
# The two rows that say how far apart the two *indexes* are before anything else.
printf '%-26s %-9s %-9s %s\n' 'tree: everything' "${REF_TREE:-refused}" "${OUR_TREE:-refused}" \
    "delta $DELTA"
one 'tree: folders' "path:$WIN_DIR folder:" "path:$DIR folder:"
for t in "${TERMS[@]}"; do
    one "$t" "path:$WIN_DIR $t" "path:$DIR $t"
done

# What the delta is made of, before the reader has to decide whether to trust it.
# Only worth the two full dumps when there is something to explain.
[ "$DELTA" != 0 ] && explain_delta

printf '\n%s row(s) disagree by more than the %s the two indexes differ by.\n' \
    "$DIFFS" "$DELTA"
[ "$STRICT" = 1 ] && [ "$DIFFS" != 0 ] && exit 1
exit 0
