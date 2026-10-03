#!/usr/bin/env bash
#
# Compare this server against voidtools' own, on one directory both of them index.
#
#   ./cmp_ref.sh                  the pinned term list, on the parent of this repo
#   ./cmp_ref.sh /mnt/c/Users/x   a different directory the reference can also see
#   ./cmp_ref.sh /mnt/c/Users/x 'foo' 'bar'   just these terms
#   ./cmp_ref.sh --strict         exit 1 on a row the index delta does not explain
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
# first and state the one difference that is expected: the reference indexes files
# that /mnt/c cannot see (Windows creates them, and NTFS has what ext4 does not),
# so it answers a slightly larger number for anything scoped to the whole
# directory. A row that differs by exactly that many is marked `= delta`, and
# anything else is marked DIFF -- which is how the term-scope rules in design 12.10
# were pinned rather than guessed. `--strict` fails on a DIFF row, i.e. on any
# disagreement the index delta does not already explain.

set -u
cd "$(dirname "$0")"

BIN=./esidx
PROBE=./etp-probe
REF_PORT=${ESIDX_REF_PORT:-21}
OUR_PORT=${ESIDX_CMP_PORT:-2199}

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
)
[ $# -gt 0 ] && TERMS=("$@")

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

# rows <port> <search> -> the row count the probe read back, empty if it got none
rows() {
    local f
    f=$(mktemp)
    {
        echo 'send USER anonymous'
        echo 'send EVERYTHING COUNT 20000'
        printf 'send EVERYTHING SEARCH %s\n' "$2"
        echo 'sendraw EVERYTHING QUERY'
        echo 'query'
        echo 'close'
    } >"$f"
    "$PROBE" "$1" "$f" 2>/dev/null >"$TMP/raw"
    rm -f "$f"
    sed -n 's/.*count=\([0-9]*\).*/\1/p' "$TMP/raw" | head -1
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

printf '\n%s row(s) disagree by more than the %s the two indexes differ by.\n' \
    "$DIFFS" "$DELTA"
[ "$STRICT" = 1 ] && [ "$DIFFS" != 0 ] && exit 1
exit 0
