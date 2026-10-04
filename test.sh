#!/usr/bin/env bash
#
# esidx regression tests.
#
#   ./test.sh                 build + run everything
#   ./test.sh -v              echo each query and its diagnostics
#   ESIDX_LOG=info ./test.sh  keep INFO timings (they go to the log dump)
#   TEST_ROOT=/ ./test.sh    index a bigger tree (slower)
#
# Runs entirely inside a mktemp dir, so the source tree stays clean.
# Results go to stdout and diagnostics to stderr; the helpers keep them apart so
# a PASS/FAIL never depends on log noise.
#
# NOTE: every query argument is quoted. `size:>1k` unquoted is a shell
# redirection, not a query -- that mistake cost us an hour.

set -u
cd "$(dirname "$0")"

BIN=./esidx
TEST_ROOT=${TEST_ROOT:-/etc}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

DIAG="$TMP/diag.log"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

PASS=0
FAIL=0
LAST=""

say() { printf '\n== %s\n' "$*"; }
ok()  { PASS=$((PASS + 1)); printf '   \033[32mok\033[0m   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '   \033[31mFAIL\033[0m %s\n' "$1"
        if [ "$#" -gt 1 ]; then printf '         %s\n' "$2"; fi; }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got [$2] want [$3]"; fi; }

# q <expr...>  -> results in $LAST, stderr appended to $DIAG, exit status in $RC
RC=0
q() {
    "$BIN" query "$DB" "$@" >"$TMP/out" 2>>"$DIAG"
    RC=$?
    LAST=$(cat "$TMP/out")
    if [ "$VERBOSE" = 1 ]; then
        printf '   $ esidx query DB %s\n' "$(printf "'%s' " "$@")"
        printf '%s' "$LAST" | sed 's/^/     | /'
    fi
    return 0
}

# result-row count
n() { printf '%s' "$1" | grep -c . ; }
# field 2 = size
col2() { printf '%s\n' "$1" | awk 'NF{print $2}' | tr '\n' ' '; }
# last field = path
paths() { printf '%s\n' "$1" | awk 'NF{print $NF}' | tr '\n' ' '; }

# build <root> <outfile> -> sets $BUILT to the entry count, echoes diagnostics
BUILT=""
build() {
    # the "indexed N entries" line goes to stderr, so capture it there
    if "$BIN" build "$1" -o "$2" >/dev/null 2>"$TMP/be"; then
        BUILT=$(sed -n 's/^indexed \([0-9]*\) entries.*/\1/p' "$TMP/be")
        grep -E '^indexed ' "$TMP/be" | sed 's/^/   /'
        cat "$TMP/be" >>"$DIAG"
        return 0
    fi
    cat "$TMP/be" >>"$DIAG"
    bad "build $1" "see $DIAG"
    BUILT=""
    return 1
}

# ------------------------------------------------------------------ fixtures

say "fixtures"

TREE="$TMP/tree"
mkdir -p "$TREE/sub1" "$TREE/sub2" "$TREE/empty"
: >"$TREE/a.txt"                     #   0 bytes
printf 'x%.0s' $(seq 1 500)       >"$TREE/b.conf"      # 500
printf 'x%.0s' $(seq 1 5000)      >"$TREE/c.conf"      # 5000
: >"$TREE/d.log"                     #   0 bytes
printf 'y%.0s' $(seq 1 200)       >"$TREE/sub1/x.conf" # 200
printf 'z%.0s' $(seq 1 100)       >"$TREE/sub2/y.txt"  # 100
ln -sf a.txt "$TREE/link.txt"        # symlink, stored via AT_SYMLINK_NOFOLLOW

# 250 nested levels below sub1: depth 252 from the root. A per-frame getdents
# buffer could not hold this on an 8 MiB stack -- the pool can.
p=""
for i in $(seq 1 250); do
    p="$p/d"; mkdir -p "$TREE/sub1/deep$p"; printf 'q' >"$TREE/sub1/deep$p/leaf.txt"
done

FIND_N=$(find "$TREE" | wc -l)
echo "   synthetic tree: $FIND_N entries, deepest path 252 levels"
echo "   real root:      $TEST_ROOT"

# ------------------------------------------------------------------ build

say "build"

DB="$TMP/root.idx"
if build "$TEST_ROOT" "$DB"; then ok "build $TEST_ROOT"; else exit 1; fi

TREE_DB="$TMP/tree.idx"
build "$TREE" "$TREE_DB" >/dev/null
expect "entry count matches find()" "$BUILT" "$FIND_N"

# ------------------------------------------------------------------ queries
# everything below runs against the synthetic tree so the expected counts are
# exact; DB switches back to the real root for the performance section at the end
DB="$TREE_DB"

say "directory tree (L0: dir children, O(1))"

q "parent:$TREE/sub1"
expect "parent: direct children only"        "$(n "$LAST")" "2"   # x.conf, deep

q "parent:$TREE/sub1" "folder:"
expect "parent: + folder: -> subdirs"         "$(n "$LAST")" "1"

q "parent:$TREE/sub1" "file:"
expect "parent: + file: -> files"             "$(n "$LAST")" "1"

q "parent:$TREE/empty"
expect "parent: of an empty dir -> nothing"   "$(n "$LAST")" "0"

q "parent:$TREE/sub1/nope"
expect "parent: on a missing dir -> nothing"    "$(n "$LAST")" "0"

say "extension filter"

q "ext:conf"
expect "ext:conf finds all three .conf"       "$(n "$LAST")" "3"

q "ext:conf;log"
expect "ext: a..b..c means OR"                "$(n "$LAST")" "4"

q "ext:CONF"
expect "ext: is case-insensitive"             "$(n "$LAST")" "3"

q "ext:conf" "sort:size:desc"
expect "ext:conf sorted by size desc"         "$(col2 "$LAST")" "5000 500 200 "

# An extension id is a byte offset into the pool of extension names, and it is 16-bit.
# A pool past 64 KB wraps those offsets onto *other* extensions, so `ext:` answers with
# rows that do not carry the extension: on a 2 500-extension fixture, 904 of the 2 500
# returned the wrong row count, in 452 aliased pairs, and ext:e...005 matched both
# f5.e...005 and f1477.e...001477. Nothing above could see it -- the flat fixture has 3
# extensions -- so the fixture here exists to overflow the pool: 2 200 x 32 bytes = 70 KB.
#
# Two assertions, because they answer different questions. The build's own invariant is
# the mechanism (every string in the pool was interned by an entry, so the pool's string
# count and the number of extensions in use must be equal); the sample is the answer.
EXTD="$TMP/exts"
mkdir -p "$EXTD"
seq 1 2200 | awk '{printf "f%d.e%030d\n", $1, $1}' | ( cd "$EXTD" && xargs touch )
EXT_DB="$TMP/exts.idx"
build "$EXTD" "$EXT_DB" >/dev/null
if grep -q 'ext pool holds' "$TMP/be"; then
    bad "an extension pool past 64 KB does not alias ids" \
        "$(grep -m1 'ext pool holds' "$TMP/be" | sed 's/^\[error \] //')"
else
    ok "an extension pool past 64 KB does not alias ids"
fi

# Each of these extensions is on exactly one file, so each must match exactly that file.
# Every 55th, 40 of them. The sample cannot miss the bug by luck: aliased pairs were 904
# of 2 500 when it was measured, so 40 draws miss with probability (1 - 0.36)^40, i.e.
# never in practice -- and the fixture above makes it deterministic anyway.
DB="$EXT_DB"
extbad=0; extfirst=""
i=0
while [ "$i" -lt 2200 ]; do
    i=$((i + 55))
    e="e$(printf '%030d' "$i")"
    q "ext:$e" "count:0"
    if [ "$(n "$LAST")" != "1" ] || [ "$(paths "$LAST")" != "$EXTD/f$i.$e " ]; then
        extbad=$((extbad + 1))
        [ -z "$extfirst" ] && extfirst="$e -> [$(paths "$LAST")]"
    fi
done
DB="$TREE_DB"
if [ "$extbad" = 0 ]; then
    ok "40 sampled extensions each match only their own file"
else
    bad "40 sampled extensions each match only their own file" \
        "$extbad of 40 wrong, first: $extfirst"
fi

# An extension used to be cut to 31 characters on the way into the index, which loses
# the tail: a query for the real extension matched nothing, and two extensions sharing
# their first 31 characters became one id and matched each other's rows. Counted on
# real trees by the build (1 entry on /usr, 709 on /work -- a 34-character extension
# under /usr/lib), so it is not a fixture artefact.
#
# The two cases below are the two wrong answers, and they are separate: the first is a
# miss, the second is a false hit. 31 characters is comfortably inside NAME_MAX, which is
# why the cut was never supposed to be reachable.
EXTL="$TMP/extlong"
mkdir -p "$EXTL"
P31=$(printf 'a%.0s' $(seq 1 31))
: >"$EXTL/one.${P31}TAILONE"
: >"$EXTL/two.${P31}TAILTWO"
: >"$EXTL/short.txt"
EXTL_DB="$TMP/extlong.idx"
build "$EXTL" "$EXTL_DB" >/dev/null

DB="$EXTL_DB"
q "ext:${P31}TAILONE" "count:0"
expect "an extension longer than 31 characters is found by its full name" \
       "$(paths "$LAST")" "$EXTL/one.${P31}TAILONE "
q "ext:${P31}TAILTWO" "count:0"
expect "...and the other one, which shares its first 31 characters, stays separate" \
       "$(paths "$LAST")" "$EXTL/two.${P31}TAILTWO "
q "ext:$P31" "count:0"
expect "a 31-character prefix of them is not an extension of anything" \
       "$(n "$LAST")" "0"
DB="$TREE_DB"

# ...and the build must report no cut at all, which is the invariant rather than one
# example of it: the counter is in ext_of(), so it reads 0 or the index is lossy. It is a
# warning, not an INFO line, so it is visible here without ESIDX_LOG -- an INFO version of
# this assertion passed on a fixture that was being cut, which is AGENTS.md 3.5 exactly.
if grep -q 'the index is lossy' "$TMP/be"; then
    bad "no extension is cut on the way into the index" \
        "$(grep -m1 'the index is lossy' "$TMP/be" | sed 's/^\[[a-z]* \] //')"
else
    ok "no extension is cut on the way into the index"
fi

say "size filter (sorted index -> bitmap)"

# NOTE: size: also matches folders -- on ext4 a directory's st_size is typically
# 4096, so every dir in the fixture passes size:>1000. That matches Everything's
# behaviour, so the assertions below pin files explicitly with file:.
q "size:>1000" "file:"
expect "size:>1000 file: -> only c.conf"     "$(n "$LAST")" "1"

q "size:>1000" "ext:conf"
expect "size + ext combine"                   "$(n "$LAST")" "1"

q "size:>1000" "ext:log"
expect "size + ext in conflict -> nothing"    "$(n "$LAST")" "0"

q "size:400..600"
expect "size:400..600 range"                  "$(n "$LAST")" "1"

q "size:<1" "file:"
expect "size:<1 catches the empty files"      "$(n "$LAST")" "2"

q "size:5000"
expect "size:5000 exact match"                "$(n "$LAST")" "1"

q "size:>1000" "folder:"
expect "size: also applies to folders"        "$([ "$(n "$LAST")" -gt 200 ] && echo yes || echo no)" "yes"

say "mtime filter"

q "dm:>2000"
expect "dm:>2000 matches everything built now" "$(n "$LAST")" "$FIND_N"

q "dm:>99999999999"
expect "dm: far future -> nothing"            "$(n "$LAST")" "0"

say "substring (L2 in-memory scan)"

q "conf"
expect "bare word matches every .conf"        "$(n "$LAST")" "3"

q "CONF"
expect "substring is case-insensitive"        "$(n "$LAST")" "3"

q "conf" "ext:log"
expect "substring + ext in conflict"          "$(n "$LAST")" "0"

say "sort"

q "ext:conf" "sort:size:asc"
expect "sort:size:asc is monotonic"           "$(col2 "$LAST")" "200 500 5000 "

q "ext:conf" "sort:size:desc"
expect "sort:size:desc is monotonic"          "$(col2 "$LAST")" "5000 500 200 "

q "ext:conf" "sort:name:asc"
expect "sort:name:asc is monotonic" \
       "$(paths "$LAST")" "$TREE/b.conf $TREE/c.conf $TREE/sub1/x.conf "

q "ext:conf" "sort:path:asc"
expect "sort:path:asc keeps every row"        "$(n "$LAST")" "3"

q "ext:conf" "sort:ext:asc"
expect "sort:ext:asc keeps every row"         "$(n "$LAST")" "3"

# A path sort over a large result set. The bug this pins is not an ordering rule
# but an allocation: sort_string() handed a path sort a fresh 64 KiB buffer per row
# and held every one of them until the sort ended, so the *address space* a query
# asked for was 64 KiB x rows -- 23 GB over an unfiltered /usr. It looks like it
# works because only the first page of each buffer is ever written, and it does not:
# where malloc refuses, sort_string() returned "", every comparison tied, and
# cmp_rec's tie-break silently turned the path sort into a name sort.
#
# So the assertion runs under a virtual-memory cap small enough that the old path
# cannot allocate at all. The expectation is order_ref, which is strcasecmp -- see
# tools/order_ref.c for why `sort -f` and `tr | sort` are each wrong in a way a
# lower-case fixture hides.
SORT="$TMP/sortbig"
mkdir -p "$SORT/a_dir" "$SORT/zdir"
( cd "$SORT" && seq 1 5000 | sed 's/^/f/' | xargs touch )
: >"$SORT/a_dir/z.txt"
: >"$SORT/b.txt"
# Case that separates the three candidate orders. `sort -f` puts Apple.txt before
# a_dir, because it orders by the original bytes; strcasecmp puts a_dir first,
# because '_' (0x5F) is below 'p'. And a non-ASCII name separates strcasecmp from
# `tr | sort`, because LC_ALL=C sort compares bytes signed and strcasecmp unsigned.
#
# zdir exists for the attribute-sort assertion below: a directory scores 0x10 and a
# plain file 0x20 (store.c esidx_win_attributes), so an attribute sort puts both
# directories first while a name sort puts zdir last. That is what lets the two be
# told apart.
UNAME=$'\u00dcnicode.txt'
: >"$SORT/Apple.txt"; : >"$SORT/banana.txt"; : >"$SORT/Cherry.txt"; : >"$SORT/$UNAME"

# One mtime for every row in this fixture. It is what lets the date sort below be a
# pure tie-break assertion: without it a date sort is ordered by when the shell got
# round to creating each file, which is not a property of the code. `touch -d @`
# rather than "they were all just created", because 5000 files can straddle a second
# boundary on a loaded machine and an assertion that depends on that is a coin flip.
# ctime cannot be pinned this way at all -- it is inode-change time -- which is why
# date_created is absent from this group and not because it behaves differently.
find "$SORT" -exec touch -d @1600000000 {} +

SORT_DB="$TMP/sortbig.idx"
build "$SORT" "$SORT_DB" >/dev/null

# The CLI's sort: used to carry its own list of keys, which drifted from the 22 the
# ETP path uses: `attributes` and `inverse_size` were missing and fell through to
# `else sort.key = SORT_NAME`, so `sort:attributes:asc` answered with a *name* sort,
# silently. It is now routed through sort_from_etp_name(), and an unknown key is an
# error instead of a name sort (design §5.3).
DB="$SORT_DB"
# the root is a directory too, so it sorts with the others -- and its *name* is the
# basename of the path it was indexed from, which is what puts it between the two
q "" "sort:attributes:asc" "count:3"
expect "sort:attributes:asc is an attribute sort, not a name sort" \
       "$(paths "$LAST")" "$SORT/a_dir $SORT $SORT/zdir "
q "" "sort:attributes:desc" "count:1"
expect "sort:attributes:desc is the other end of the attribute order" \
       "$(paths "$LAST")" "$SORT/$UNAME "
if "$BIN" query "$SORT_DB" "" "sort:nosuchkey:asc" >/dev/null 2>"$TMP/err"; then
    bad "an unknown sort: key is refused" "it answered $(cat "$TMP/err")"
else
    grep -q 'unknown sort key' "$TMP/err" \
        && ok "an unknown sort: key is refused" \
        || bad "an unknown sort: key is refused" "$(cat "$TMP/err")"
fi
DB="$TREE_DB"

# 5007 rows x 64 KiB is 321 MB of address space; the cap leaves room for the index
# itself (~1 MB) and nothing else. Under a sanitiser build the cap cannot be used at
# all -- AddressSanitizer reserves terabytes of address space before main() -- so the
# uncapped ordering assertion below runs in both builds and only the cap is skipped.
SORT_CAP_KB=65536
find "$SORT" | awk '{print $NF}' >"$TMP/sort.want"
if ldd "$BIN" 2>/dev/null | grep -q 'libasan\|libubsan'; then
    printf '   \033[33mskip\033[0m the memory-capped path sort under the sanitiser build\n'
else
    ( ulimit -v "$SORT_CAP_KB" 2>/dev/null
      "$BIN" query "$SORT_DB" "sort:path:asc" "count:0" 2>/dev/null ) \
        | awk '{print $NF}' >"$TMP/sort.got"
    if ./order-ref -c "$TMP/sort.got" "$TMP/sort.want" >"$TMP/ord.msg" 2>&1; then
        ok "a path sort of 5007 rows keeps path order under a 64 MiB cap"
    else
        bad "a path sort of 5007 rows keeps path order under a 64 MiB cap" \
            "$(cat "$TMP/ord.msg")"
    fi
fi

# the invariant, without the cap
"$BIN" query "$SORT_DB" "sort:path:asc" "count:0" 2>/dev/null \
    | awk '{print $NF}' >"$TMP/sort.got"
if ./order-ref -c "$TMP/sort.got" "$TMP/sort.want" >"$TMP/ord.msg" 2>&1; then
    ok "a path sort agrees with strcasecmp on the full result set"
else
    bad "a path sort agrees with strcasecmp on the full result set" "$(cat "$TMP/ord.msg")"
fi

# A name sort is an integer compare on a rank built in finalize, so its order has to
# be strcasecmp's exactly -- the one the sort produced before the rank existed,
# because a client pages by OFFSET and cmp_ref.sh compares ordered result sets.
# Checked over every row, not a page: a rank bug puts one row in the wrong place
# among thousands, and a page would not see it.
"$BIN" query "$SORT_DB" "sort:name:asc" "count:0" 2>/dev/null \
    | awk -F/ '{print $NF}' >"$TMP/nm.got"
find "$SORT" | awk -F/ '{print $NF}' >"$TMP/nm.want"
if ./order-ref -c "$TMP/nm.got" "$TMP/nm.want" >"$TMP/ord.msg" 2>&1; then
    ok "a name sort is strcasecmp order over every row"
else
    bad "a name sort is strcasecmp order over every row" "$(cat "$TMP/ord.msg")"
fi

# ...and the descending direction, a different line through cmp_rec (`desc ? -r : r`)
"$BIN" query "$SORT_DB" "sort:name:desc" "count:0" 2>/dev/null \
    | awk -F/ '{print $NF}' >"$TMP/nm.got"
if ./order-ref -c "$TMP/nm.got" "$TMP/nm.want" >/dev/null 2>&1; then
    bad "sort:name:desc must differ from ascending" "it did not"
else
    ok "sort:name:desc differs from ascending"
fi
tac "$TMP/nm.got" >"$TMP/nm.rev"
if ./order-ref -c "$TMP/nm.rev" "$TMP/nm.want" >"$TMP/ord.msg" 2>&1; then
    ok "sort:name:desc is the exact reverse of ascending"
else
    bad "sort:name:desc is the exact reverse of ascending" "$(cat "$TMP/ord.msg")"
fi

# Every sort that is not a name sort ends in the same place: cmp_rec's display-name
# tie-break. It orders the rows the primary key cannot separate, and on a real tree
# that is most of them -- over an unfiltered /usr a whole directory shares one size
# and a whole minute shares one mtime, so the four numeric keys land in it on nearly
# every comparison (design §10). It is part of the order contract, so it is pinned.
#
# Each case is built so the *primary* key is constant on every row, which makes the
# tie-break the entire order and lets find(1) supply the expectation (AGENTS.md 3.2):
# every file in $SORT is 0 bytes, every row's mtime was pinned above, and the
# extension case drops the only files that have one. A case with a varying primary
# would want a two-column oracle, and the column under test here is the constant one.
find "$SORT" -type f         | awk -F/ '{print $NF}' >"$TMP/tb_files.want"
find "$SORT" ! -name '*.txt' | awk -F/ '{print $NF}' >"$TMP/tb_noext.want"

# tie_ok <label> <sort key> <want> <terms...>: nothing but the tie-break decides the
# order, so order_ref over the same set of names is the expectation.
tie_ok() {
    _tl=$1; _tk=$2; _tw=$3; shift 3
    q "$@" "sort:$_tk" "count:0"
    printf '%s\n' "$LAST" | awk -F/ '{print $NF}' >"$TMP/tb.got"
    if ./order-ref -c "$TMP/tb.got" "$_tw" >"$TMP/ord.msg" 2>&1; then
        ok "$_tl"
    else
        bad "$_tl" "$(cat "$TMP/ord.msg")"
    fi
}

DB="$SORT_DB"      # every case below reads $SORT_DB; the name assertions above named
                   # it explicitly because they run with DB pointing at $TREE_DB
tie_ok "a size sort breaks its ties on the display name" \
       size:ascending "$TMP/tb_files.want" file:
tie_ok "an attribute sort breaks its ties on the display name" \
       attributes:ascending "$TMP/tb_files.want" file:
tie_ok "an extension sort breaks its ties on the display name" \
       ext:ascending "$TMP/tb_noext.want" '!ext:txt'
tie_ok "a date_modified sort breaks its ties on the display name" \
       date_modified:ascending "$TMP/nm.want"

# ...and through the other direction, which is a different line of cmp_rec. Expectation
# is the reverse of the *ascending answer*, compared byte for byte rather than through
# order-ref: -c sorts the file it is given, so it cannot express "descending" at all.
q "file:" "sort:size:ascending" "count:0"
printf '%s\n' "$LAST" | awk -F/ '{print $NF}' >"$TMP/tb_asc.got"
tac "$TMP/tb_asc.got" >"$TMP/tb_asc.rev"
q "file:" "sort:size:descending" "count:0"
printf '%s\n' "$LAST" | awk -F/ '{print $NF}' >"$TMP/tb.got"
if cmp -s "$TMP/tb.got" "$TMP/tb_asc.rev"; then
    ok "sort:size:descending is the exact reverse of ascending, tie-break included"
else
    bad "sort:size:descending is the exact reverse of ascending, tie-break included" \
        "$(cmp "$TMP/tb.got" "$TMP/tb_asc.rev" 2>&1 | head -3)"
fi

# Four names that strcasecmp calls equal, which is the case the tie-break exists for.
#
# This was a real order bug, not a hypothetical one. A name sort compares ranks, so it
# reaches the tie-break on exactly these rows -- and the tie-break was reading the
# *unfolded* display name, because the ranked path skipped the copy that every other
# key folds into the arena. memcmp over those raw bytes is not strcasecmp, so
# "README" and "readme" came out in byte order instead of tying and falling to the id,
# and the no-rank fallback path (which folds `dn` like every other key) disagreed with
# the ordinary one about the same four rows.
#
# Nothing caught it because every order fixture had distinct names, so the tie-break
# was unreachable, and a bug in a level of a multi-level sort is invisible until a
# fixture reaches it.
#
# The expectation is id order, read through find(1): the scanner hands out ids in
# getdents order and find lists the same directory in the same order. That is the only
# place the id is observable from outside the process.
CASE="$TMP/case"
mkdir -p "$CASE"
: >"$CASE/Foo"; : >"$CASE/foo"; : >"$CASE/FOO"; : >"$CASE/fOo"
CASE_DB="$TMP/case.idx"
build "$CASE" "$CASE_DB" >/dev/null
find "$CASE" -mindepth 1 -maxdepth 1 | awk -F/ '{print $NF}' \
    | grep -E '^[Ff][Oo][Oo]$' >"$TMP/ids.want"
"$BIN" query "$CASE_DB" "" "sort:name:ascending" "count:0" 2>/dev/null \
    | awk -F/ '{print $NF}' | grep -E '^[Ff][Oo][Oo]$' >"$TMP/nmcase.got"
if cmp -s "$TMP/nmcase.got" "$TMP/ids.want"; then
    ok "names differing only in case tie, and the id decides"
else
    bad "names differing only in case tie, and the id decides" \
        "$(cmp "$TMP/nmcase.got" "$TMP/ids.want" 2>&1 | head -3)"
fi

# ...and the fixture is only worth anything if the two rules actually disagree on it.
# When the filesystem happens to hand these four out in raw byte order the assertion
# above cannot fail, so that is checked rather than assumed -- a guard that silently
# stops guarding is worse than no assertion (AGENTS.md 3.5). LC_ALL=C sort *is* the old
# rule: strcmp semantics on the bytes as stored, which is what cmp_folded() saw.
LC_ALL=C sort "$TMP/ids.want" >"$TMP/bytes.want"
if cmp -s "$TMP/ids.want" "$TMP/bytes.want"; then
    bad "the case fixture separates byte order from id order" \
        "find(1) lists these four in raw byte order, so the assertion above is vacuous"
else
    ok "the case fixture separates byte order from id order"
fi
DB="$TREE_DB"


say "offset / count"

q "ext:conf" "count:1"
expect "count:1 returns one row"              "$(n "$LAST")" "1"

q "ext:conf" "count:1" "offset:1"
expect "count:1 offset:1 returns the second"  "$(paths "$LAST")" "$TREE/c.conf "

q "ext:conf" "count:5" "offset:99"
expect "offset past the end -> nothing"       "$(n "$LAST")" "0"

say "cross-check against find(1)"

q "size:>1000" "file:"
expect "size:>1000 file: == find -type f -size +1k" \
       "$(n "$LAST")" "$(find "$TREE" -type f -size +1k | wc -l)"

q "ext:conf" "file:"
expect "ext:conf file: == find -name '*.conf' -type f" \
       "$(n "$LAST")" "$(find "$TREE" -type f -name '*.conf' | wc -l)"

q "parent:$TREE/sub2"
expect "parent: == find -mindepth 1 -maxdepth 1" \
       "$(n "$LAST")" "$(find "$TREE/sub2" -mindepth 1 -maxdepth 1 | wc -l)"

# ------------------------------------------------------------------- syntax
#
# Everything above pins the *index*: the numbers come from find(1). Everything
# below pins the *language*, against a second, deliberately flat fixture whose
# contents are known exactly -- a syntax regression and an index regression look
# identical from the outside but have nothing to do with each other, and the deep
# chain in $TREE makes hand-written counts unreadable.
#
# FLAT (13 entries = 5 dirs + 8 files):
#   dirs  FLAT sub1 sub2 empty sub1/deep
#   txt   a.txt sub2/y.txt sub1/deep/leaf.txt
#   conf  b.conf c.conf sub1/x.conf
#   log   d.log
#   none  .hidden      (a leading dot is not an extension)
# NOTE the root entry's own name is the path it was given, not "flat", so a query
# on the root's name or length behaves differently from a real directory's.

FLAT="$TMP/flat"
mkdir -p "$FLAT/sub1/deep" "$FLAT/sub2" "$FLAT/empty"
: >"$FLAT/a.txt"
printf 'x%.0s' $(seq 1 500)  >"$FLAT/b.conf"
printf 'x%.0s' $(seq 1 5000) >"$FLAT/c.conf"
: >"$FLAT/d.log"
printf 'y%.0s' $(seq 1 200)  >"$FLAT/sub1/x.conf"
printf 'z%.0s' $(seq 1 100)  >"$FLAT/sub2/y.txt"
printf 'q' >"$FLAT/sub1/deep/leaf.txt"
: >"$FLAT/.hidden"
FLAT_DB="$TMP/flat.idx"
build "$FLAT" "$FLAT_DB" >/dev/null
expect "flat fixture has 13 entries" "$BUILT" "13"
DB="$FLAT_DB"

say "query language: L0 structural"

q "folder:"      ; expect "folder:"                    "$(n "$LAST")" "5"
q "file:"        ; expect "file:"                      "$(n "$LAST")" "8"
q "!folder:"     ; expect "!folder:"                   "$(n "$LAST")" "8"
q "root:"        ; expect "root: is the top level"     "$(n "$LAST")" "8"
q "parent:$FLAT" ; expect "parent:"                    "$(n "$LAST")" "8"
q "depth:1"      ; expect "depth:1"                    "$(n "$LAST")" "8"
q "depth:2"      ; expect "depth:2"                    "$(n "$LAST")" "3"
q "depth:>1"     ; expect "depth:>1"                   "$(n "$LAST")" "4"

say "query language: L1 scalar and enum"

q "ext:conf"          ; expect "ext:conf"              "$(n "$LAST")" "3"
q "ext:conf;txt"      ; expect "ext: is a ;-separated OR" "$(n "$LAST")" "6"
q "ext:*.conf"        ; expect "ext: tolerates a wildcard" "$(n "$LAST")" "3"
q "ext:CONF"          ; expect "ext: folds case"       "$(n "$LAST")" "3"
q "size:>1k" "file:"  ; expect "size:>1k"              "$(n "$LAST")" "1"
q "size:<1k" "file:"  ; expect "size:<1k"              "$(n "$LAST")" "7"
q "size:100..1000"    ; expect "size:a..b"             "$(n "$LAST")" "3"
q "size:empty" "file:"; expect "size:empty is the 0-byte constant" "$(n "$LAST")" "3"
q "dm:>2000-01-01"    ; expect "dm: with an ISO date"  "$(n "$LAST")" "13"
q "dm:<2000-01-01"    ; expect "dm: before everything" "$(n "$LAST")" "0"
q "dc:>2000-01-01"    ; expect "dc: has its own index" "$(n "$LAST")" "13"
q "empty:"            ; expect "empty:"                "$(n "$LAST")" "4"
q "attrib:h"          ; expect "attrib:h finds the dot file" "$(n "$LAST")" "1"
q "attrib:!h"         ; expect "attrib:!h"             "$(n "$LAST")" "12"
q "child-count:0"     ; expect "child-count:0"         "$(n "$LAST")" "9"
q "len:4"             ; expect "len: (root name is its path)" "$(n "$LAST")" "3"

say "query language: operators"

q "ext:conf | ext:log"           ; expect "| is OR"           "$(n "$LAST")" "4"
q "ext:conf !file:"              ; expect "! is AND-NOT"      "$(n "$LAST")" "0"
q "!folder: ext:txt"             ; expect "juxtaposition is AND" "$(n "$LAST")" "3"
q "<ext:conf ext:log>"           ; expect "<> groups, AND inside" "$(n "$LAST")" "0"
q "ext:conf | <ext:log ext:txt>" ; expect "grouping inside OR" "$(n "$LAST")" "3"
q "ext:<conf log>"               ; expect "fn:<a b> is an AND list" "$(n "$LAST")" "0"
q "ext:<conf | log>"             ; expect "fn:<a|b> is an OR list"  "$(n "$LAST")" "4"
q "ext:<conf;log>"               ; expect "fn:<a;b> is an OR list"  "$(n "$LAST")" "4"
q "!!folder:"                    ; expect "!! collapses"      "$(n "$LAST")" "5"

say "query language: text and modifiers"

q "conf"                     ; expect "a bare word is a substring" "$(n "$LAST")" "3"
q "CONF"                     ; expect "substring ignores case"     "$(n "$LAST")" "3"
q "name:x.conf"              ; expect "name:"                      "$(n "$LAST")" "1"
q "path:sub1"                ; expect "path: searches the full path" "$(n "$LAST")" "4"
q "whole:b.conf"             ; expect "whole:"                     "$(n "$LAST")" "1"
q "startwith:sub"            ; expect "startwith: no word boundary" "$(n "$LAST")" "2"
q "prefix:sub"               ; expect "prefix: needs a word boundary" "$(n "$LAST")" "0"
q "endwith:onf"            ; expect "endwith: no word boundary" "$(n "$LAST")" "3"
q "suffix:onf"             ; expect "suffix: needs a word boundary" "$(n "$LAST")" "0"
q "*.conf"                   ; expect "* is a wildcard over the whole name" "$(n "$LAST")" "3"
q "?.conf"                   ; expect "? is one character"        "$(n "$LAST")" "3"

# The next three blocks are the term-scope rules, pinned against the reference
# server rather than against our reading of the code (the numbers in the comments
# are `etp-probe` against voidtools' own server on :21, over one directory that
# both servers index).
#
# `*` means "any characters, 0 or more" (everything-syntax.md L10-15), so a
# trailing `*` has to be able to match the *empty* remainder -- otherwise the only
# patterns that work are the ones whose literal ends at the end of the name:
#   reference :21   *ZZZ*  ->  423 rows, incl. aaaZZZbbb.txt
#   this build        *ZZZ*  ->  0
# Here `on` sits strictly inside b.conf / c.conf / sub1/x.conf, while `conf` sits
# at the end -- so the pair is the whole bug: same shape, position is all that
# differs.
q "*conf*"                   ; expect "*conf* (literal at the end) works"  "$(n "$LAST")" "3"
q "*on*"                     ; expect "*on* (literal inside) works too"   "$(n "$LAST")" "3"
q "name:*on*"                ; expect "name: with a trailing *"           "$(n "$LAST")" "3"
# `name:` is the filename and a bare word is the filename too. Everything does
# not fall back to the path, so a term naming one entry does not match everything
# below a directory of that name:
#   reference :21   name:sub1  ->  1 (the directory)
#   this build        name:sub1  ->  3 (the directory and both things in it)
q "name:sub1"                ; expect "name: is the filename, not the path" "$(n "$LAST")" "1"
q "sub1"                     ; expect "a bare word is too"        "$(n "$LAST")" "1"
q "regex:sub1"               ; expect "and so is an unanchored regex" "$(n "$LAST")" "1"
q "path:sub1"                ; expect "path: still reads the path" "$(n "$LAST")" "4"
# ...and the scope is the filename for a wildcard term too, which is the other
# half of the same rule: `*sub1*` is the one directory, not the 4 entries whose
# path mentions it.
q "*sub1*"                   ; expect "a wildcard term is the filename as well" "$(n "$LAST")" "1"

# A separator in the value switches the term to the path -- Everything's own
# widening, measured on one directory against voidtools' server on :21 (the numbers
# in the comments are the reference's; the counts below are the fixture's, which is
# flat and small so a reader can check them by hand):
#   esidx + sep + *   ->  38      esidx\main.c ->  1      folder: esidx + sep + * ->  3
#   sidx + sep + *    ->   0      *esidx/main.c ->  1      path:*/main.c ->  1
# 38 is what `find <dir>/esidx -mindepth 1 -maxdepth 1 | wc -l` says, so the star is
# the direct children and not the subtree; and `sidx` -> 0 is what says a wildcard
# may not begin inside a component -- while the literal `sidx/main.c` is 1, because
# a literal with a separator is a substring and not a pattern.
q 'sub1/x.conf'              ; expect "a separator in the value reads the path" "$(n "$LAST")" "1"
q 'sub1\*.conf'              ; expect "a backslash separates there too"  "$(n "$LAST")" "1"
q 'sub1/*'                   ; expect "a star stops at the next separator" "$(n "$LAST")" "2"
q 'ub1/*'                    ; expect "a wildcard may not start mid-component" "$(n "$LAST")" "0"
q 'ub1/x.conf'               ; expect "but a literal may: it is a substring" "$(n "$LAST")" "1"
# `path:` + a value that *starts* with a star is Everything's contains form -- the
# one shape where a star crosses a separator. Reference, same directory:
#   path:*esidx*   ->  280 = every path containing esidx (ours 268: the reference's
#                     index holds 12 entries in that tree that WSL cannot see)
#   path:*PC*      -> 6590 = everything, because every path contains "PC"
#   path:*esidx    ->    2 = the paths *ending* in esidx, so no trailing star still
#                     means ends-with
#   path:*PC/esidx*->    1 = ends_with("PC/esidx"), because a value with a separator
#                     in it is a fragment and a fragment's trailing star cannot cross
q 'path:*x.conf'             ; expect "path: with a leading star"      "$(n "$LAST")" "1"
q 'path:*sub1*'              ; expect "a leading star makes it contains" "$(n "$LAST")" "4"
q 'path:*sub1/x.conf'        ; expect "but a fragment stays anchored"  "$(n "$LAST")" "1"
q 'path:*conf'               ; expect "no trailing star means ends-with" "$(n "$LAST")" "3"
q 'folder: sub1/*'           ; expect "and it combines with folder:"    "$(n "$LAST")" "1"
q "regex:^b.*conf\$"         ; expect "regex:"                    "$(n "$LAST")" "1"
q 'path:regex:sub1/deep$'    ; expect "modifiers stack in the value" "$(n "$LAST")" "1"
q 'path:regex:[a-z]\.conf$'  ; expect "regex classes and escapes"  "$(n "$LAST")" "3"
q "case:regex:^B"            ; expect "case: is case SENSITIVE"    "$(n "$LAST")" "0"
q "nocase:regex:^B"          ; expect "nocase: overrides the default" "$(n "$LAST")" "1"
q "ww:conf"                  ; expect "ww:"                       "$(n "$LAST")" "3"
q "child:b.conf"             ; expect "child:<expr>"              "$(n "$LAST")" "1"

# ------------------------------------------------------- trigram prefilter
#
# design §5.2, P4: a name trigram index narrows the candidate set before the
# matcher pass, and text_match() still decides every row. So this whole block is
# an invariance check -- every answer must come out identical with the prefilter
# present and absent -- and the shapes below are the ones where a prefilter that
# fired anyway would be *faster and wrong*: a literal under the 3-byte threshold
# (which yields no trigram at all, ref A9), and the modifiers that change the
# subject (stem:, path:) or the folding (ignorepunc:, ignorews:, diacritics:).
#
# Only the last two assertions fail without the index, and that is the point: an
# optimisation that changes no answer can only be checked by watching it run, so
# it has to leave a line in the log (AGENTS.md 4.1).
say "trigram prefilter (design §5.2)"

# the threshold: three bytes is a trigram, two is a full scan
q "onf"       ; expect "a 3-byte literal"                "$(n "$LAST")" "3"
q "on"        ; expect "a 2-byte literal falls back"     "$(n "$LAST")" "3"
q "*nf*"      ; expect "a 2-byte run inside stars, too"  "$(n "$LAST")" "3"
q "*on*"      ; expect "the same run, unbracketed"       "$(n "$LAST")" "3"
# a wildcard is anchored to the whole filename, so narrowing must not turn one
# into a contains test: nothing here is *named* on...
q "on*"       ; expect "an anchored star is not a substring" "$(n "$LAST")" "0"

# the shapes it has to refuse, each still answering exactly as before
q "case:conf"       ; expect "case: is left to the matcher"  "$(n "$LAST")" "3"
q "ignorepunc:conf" ; expect "ignorepunc: is left to the matcher" "$(n "$LAST")" "3"
q "ignorews:conf"   ; expect "ignorews: is left to the matcher"   "$(n "$LAST")" "3"
q "diacritics:conf" ; expect "diacritics: is left to the matcher" "$(n "$LAST")" "3"
q "stem:conf"       ; expect "stem: reads the truncated name" "$(n "$LAST")" "0"
q "path:sub1"       ; expect "path: reads the path, not the name" "$(n "$LAST")" "4"
q "regex:^b.*conf\$" ; expect "regex: is left to the engine"   "$(n "$LAST")" "1"

# ...and the shapes it does take: any case-insensitive name term whose longest
# literal run is 3 bytes or more
q "CONF"       ; expect "the literal is folded, not the answer" "$(n "$LAST")" "3"
q "*.conf"     ; expect "a trailing star keeps the run"         "$(n "$LAST")" "3"
q "*conf*"     ; expect "a leading star keeps the run"          "$(n "$LAST")" "3"
q "whole:CONF" ; expect "whole: is still a strcmp, so 0"        "$(n "$LAST")" "0"
q "endwith:onf"; expect "endwith: uses the whole pattern"       "$(n "$LAST")" "3"
q "name:onf"   ; expect "name: uses the whole pattern"          "$(n "$LAST")" "3"

# Trigrams are bytes, not codepoints (ref A13), so a CJK name yields them too --
# which is the whole reason for the byte rule. Its own fixture, because the flat
# one is counted by every assertion above and by the incremental section.
UNI="$TMP/uni"
CN=$'\u4e2d\u6587\u6587\u4ef6.txt'      # 中文文件.txt
UE=$'\u00dcn\u00efc\u00f6d\u00e9.txt'  # Ünïcödé.txt
mkdir -p "$UNI"
: >"$UNI/$CN"; : >"$UNI/$UE"; : >"$UNI/plain.txt"; : >"$UNI/short"; : >"$UNI/ab"
UNI_DB="$TMP/uni.idx"
build "$UNI" "$UNI_DB" >/dev/null
expect "the unicode fixture has 6 entries" "$BUILT" "6"
DB="$UNI_DB"
q "$CN"                ; expect "a CJK name matches itself"        "$(n "$LAST")" "1"
q "$(printf '\346\226\207')" ; expect "one CJK char is 3 bytes"        "$(n "$LAST")" "1"
q "pla"                ; expect "a 3-byte run in an ASCII name"    "$(n "$LAST")" "1"
q "$(printf '\303\234n\303\257')" ; expect "non-ASCII bytes match verbatim" "$(n "$LAST")" "1"
q "$(printf '\303\274')" ; expect "but are not case-folded"          "$(n "$LAST")" "0"
q "ab"                 ; expect "a 2-byte name still matches"       "$(n "$LAST")" "1"
DB="$FLAT_DB"

# it ran, and it narrowed: 13 candidates in, the 3 rows whose name holds "onf"
ESIDX_LOG=debug "$BIN" query "$FLAT_DB" "endwith:onf" >/dev/null 2>"$TMP/err"
cat "$TMP/err" >>"$DIAG"
if grep -q "trigram prefilter 'onf': 13 -> 3" "$TMP/err"; then
    ok "the prefilter ran and narrowed the candidate set"
else
    bad "the prefilter ran and narrowed the candidate set" \
        "$(grep -F 'trigram prefilter' "$TMP/err" || echo 'no prefilter line in the log')"
fi

# and it did NOT run for a shape it must refuse -- otherwise "narrowed" above
# would only mean it is always on
ESIDX_LOG=debug "$BIN" query "$FLAT_DB" "path:sub1" >/dev/null 2>"$TMP/err"
cat "$TMP/err" >>"$DIAG"
if grep -q 'trigram prefilter' "$TMP/err"; then
    bad "the prefilter stays off for a path term" "$(grep -F 'trigram prefilter' "$TMP/err")"
else
    ok "the prefilter stays off for a path term"
fi

say "query language: macros and unsupported functions"

q "type:document"   ; expect "type:document"              "$(n "$LAST")" "3"
q "type:picture"    ; expect "type: with no members here" "$(n "$LAST")" "0"
q "type:nosuchtype" ; expect "type: unknown name -> nothing" "$(n "$LAST")" "0"
q "content:xyz"     ; expect "content: parses, matches nothing" "$(n "$LAST")" "0"
q "si:xyz"          ; expect "si: is rejected at execution" "$(n "$LAST")" "0"
q "dupe:"           ; expect "dupe: is not supported yet" "$(n "$LAST")" "0"

say "query language: errors are reported, never swallowed"

for bad in '<a' 'a |' '"unterminated' 'ext:conf )'; do
    "$BIN" query "$FLAT_DB" "$bad" >/dev/null 2>"$TMP/err"
    if [ $? -ne 0 ] && grep -q 'parse error' "$TMP/err"; then
        ok "rejects '$bad'"
    else
        bad "rejects '$bad'" "$(cat "$TMP/err")"
    fi
    cat "$TMP/err" >>"$DIAG"
done

DB="$TREE_DB"

# ------------------------------------------------------------------ errors


say "error handling"

"$BIN" query "$TMP/no-such.idx" >/dev/null 2>>"$DIAG"
[ $? -ne 0 ] && ok "missing snapshot exits non-zero" \
             || bad "missing snapshot exits non-zero"

printf 'definitely not an esidx file' >"$TMP/junk.idx"
"$BIN" query "$TMP/junk.idx" >/dev/null 2>>"$DIAG"
[ $? -ne 0 ] && ok "corrupt snapshot exits non-zero" \
             || bad "corrupt snapshot exits non-zero"

"$BIN" build "$TMP/no-such-root" -o "$TMP/x.idx" >/dev/null 2>>"$DIAG"
[ $? -ne 0 ] && ok "build of a missing root exits non-zero" \
             || bad "build of a missing root exits non-zero"

"$BIN" >/dev/null 2>&1
[ $? -ne 0 ] && ok "no subcommand exits non-zero" || bad "no subcommand exits non-zero"

# ------------------------------------------------------------------ depth

say "deep recursion"

DEEP_DB="$TMP/deep.idx"
if "$BIN" build "$TREE/sub1/deep" -o "$DEEP_DB" >/dev/null 2>"$TMP/be"; then
    DEEP_N=$(sed -n 's/^indexed \([0-9]*\) entries.*/\1/p' "$TMP/be")
    cat "$TMP/be" >>"$DIAG"
    DEEP_FIND=$(find "$TREE/sub1/deep" | wc -l)
    expect "251 levels deep indexed without stack overflow" "$DEEP_N" "$DEEP_FIND"
else
    cat "$TMP/be" >>"$DIAG"
    bad "deep tree build" "see $TMP/be"
fi

# one level past the cap must be reported, not silently dropped
TOODEEP="$TMP/toodeep"
p=""
for i in $(seq 1 600); do p="$p/d"; mkdir -p "$TOODEEP$p"; done
if "$BIN" build "$TOODEEP" -o "$TMP/td.idx" >/dev/null 2>"$TMP/err"; then
    grep -q 'depth cap' "$TMP/err" \
        && ok "hitting the depth cap is warned about" \
        || bad "hitting the depth cap is warned about" "$(cat "$TMP/err")"
else
    bad "build past the depth cap" "see $TMP/err"
fi

# ------------------------------------------------------------- incremental
#
# design §7. Every expected number here comes from find(1) on the fixture, not
# from running esidx -- the same rule as the rest of the suite, and the only one
# that can tell "the refresh works" from "the refresh is self-consistent".
#
# The fixture is separate from $TREE because these tests mutate it; the counts
# the sections above pin would not survive that, and a test that breaks its
# neighbours is worse than no test.

say "incremental refresh (design §7)"

INC="$TMP/inc"
mkdir -p "$INC/a/b" "$INC/c"
printf 'x%.0s' $(seq 1 100)  >"$INC/one.txt"
printf 'y%.0s' $(seq 1 200)  >"$INC/two.conf"
printf 'z%.0s' $(seq 1 300)  >"$INC/a/three.txt"
printf 'w%.0s' $(seq 1 400)  >"$INC/a/b/four.log"
# a fixed old date, so that "did the refresh notice the mtime change" is a
# question with a literal answer rather than one about the current second
touch -d '2020-01-01 12:00' "$INC/two.conf"

INC_DB="$TMP/inc.idx"
build "$INC" "$INC_DB" >/dev/null

# every entry the index holds must equal every entry on disk
inc_find() { find "$INC" | wc -l; }

# Total, and the dir/file split. The split is what pins the type bitmaps: a
# removed row that is still counted as a file, or an added directory that never
# reaches the folders bitmap, leaves the totals alone and shows up here. Both
# numbers come from find(1).
inc_sync() {
    q ""
    expect "index matches find(1) after: $1" "$(n "$LAST")" "$(inc_find)"
    q "folder:"; D=$(n "$LAST")
    q "file:";   F=$(n "$LAST")
    expect "dir/file split matches find after: $1" "$D/$F" \
        "$(find "$INC" -type d | wc -l)/$(find "$INC" -type f | wc -l)"
}

DB="$INC_DB"

# 1. nothing changed. The pass must cost one stat per directory and stop.
u() { "$BIN" update "$INC_DB" "$@" >/dev/null 2>>"$DIAG"; }
if u; then ok "update with no changes succeeds"; else bad "update with no changes"; fi
q ""
expect "no-op refresh keeps every entry" "$(n "$LAST")" "$(inc_find)"
q "dm:2020-01-01..2020-01-02"
expect "the fixed-date file is found by dm:" "$(n "$LAST")" "1"

# 2. a new name in a directory whose stamp moved
printf 'n%.0s' $(seq 1 50) >"$INC/new.txt"
u
q "parent:$INC file:"
expect "a created file is indexed" "$(n "$LAST")" \
    "$(find "$INC" -mindepth 1 -maxdepth 1 -type f | wc -l)"
inc_sync "create"

# 3. a deletion
rm "$INC/one.txt"
u
q "one.txt"
expect "a deleted file is gone from a text search" "$(n "$LAST")" "0"
q "ext:txt"
expect "a deleted file is gone from its extension bitmap" "$(n "$LAST")" \
    "$(find "$INC" -type f -name '*.txt' | wc -l)"
inc_sync "delete"

# 4. a rename: on disk that is a delete plus a create, and the reconcile has to
#    agree with find about which
mv "$INC/two.conf" "$INC/renamed.dat"
u
q "renamed.dat"
expect "the new name is indexed" "$(n "$LAST")" "1"
q "two.conf"
expect "the old name is not" "$(n "$LAST")" "0"
q "ext:dat"
expect "the extension followed the rename" "$(n "$LAST")" "1"
inc_sync "rename"

# A name sort after a reconcile. The rank is a *sorted position* (design §10), so a
# name the index has never seen has nowhere to go until the order is recomputed --
# which esidx_update() does once per pass that added something. Without that the new
# name sorts last instead of between renamed.dat and three.txt, and every row count
# in the suite still passes. Oracle is find(1) read through order_ref.
q "" "sort:name:asc"
printf '%s\n' "$LAST" | awk -F/ '{print $NF}' >"$TMP/incnm.got"
find "$INC" | awk -F/ '{print $NF}' >"$TMP/incnm.want"
if ./order-ref -c "$TMP/incnm.got" "$TMP/incnm.want" >"$TMP/ord.msg" 2>&1; then
    ok "a name sort after a rename is still in name order"
else
    bad "a name sort after a rename is still in name order" "$(cat "$TMP/ord.msg")"
fi

# 5. a file replaced by a directory of the same name. The flags are baked into
#    the type and ext bitmaps, so this is the case that catches a touch() which
#    only refreshes the numeric columns.
rm "$INC/a/three.txt"
mkdir -p "$INC/a/three.txt"
printf 'i' >"$INC/a/three.txt/inner.txt"
u
q "parent:$INC/a/three.txt"
expect "file -> directory is walked as a directory (B5)" "$(n "$LAST")" "1"
q "parent:$INC/a folder:"
expect "and the type bitmap agrees with find" "$(n "$LAST")" \
    "$(find "$INC/a" -mindepth 1 -maxdepth 1 -type d | wc -l)"
q "parent:$INC/a file:"
expect "with no file left under a/" "$(n "$LAST")" \
    "$(find "$INC/a" -mindepth 1 -maxdepth 1 -type f | wc -l)"
inc_sync "file -> dir"

# 6. a directory replaced by a file
rm -rf "$INC/c"
printf 'c' >"$INC/c"
u
q "parent:$INC folder:"
expect "dir -> file leaves one folder" "$(n "$LAST")" \
    "$(find "$INC" -mindepth 1 -maxdepth 1 -type d | wc -l)"
q "parent:$INC file:"
expect "and one more file" "$(n "$LAST")" \
    "$(find "$INC" -mindepth 1 -maxdepth 1 -type f | wc -l)"
inc_sync "dir -> file"

# 7. a new subtree, then removing one wholesale
mkdir -p "$INC/deep/x/y"
printf 'q' >"$INC/deep/x/y/z.txt"
u
q "parent:$INC/deep/x/y"
expect "a new subtree is walked (B5)" "$(n "$LAST")" "1"
inc_sync "new subtree"

rm -rf "$INC/deep"
u
q "z.txt"
expect "rm -rf leaves nothing behind" "$(n "$LAST")" "0"
q "parent:$INC/deep"
expect "and the removed subtree's row is gone" "$(n "$LAST")" "0"
inc_sync "rm -rf"

# 8. two hard links to one inode, so both rows have the same size and the same
#    extension. Matching a stored row to a directory entry by inode would collapse
#    them into one; by name it cannot.
ln "$INC/new.txt" "$INC/hard.txt"
u
q "size:50 file:"
expect "a hard-linked pair is two rows" "$(n "$LAST")" \
    "$(find "$INC" -type f -size 50c | wc -l)"
q "ext:txt"
expect "and both are in the extension bitmap" "$(n "$LAST")" \
    "$(find "$INC" -type f -name '*.txt' | wc -l)"
inc_sync "hard link"

# 9. attribute changes. This is the case the cheap pass provably cannot see, and
#    the test says so rather than leaving it as a caveat in a comment: a file's
#    content changes its own mtime, not its parent's.
printf 'x%.0s' $(seq 1 5000) >"$INC/new.txt"
u
q "size:>1000 file:"
expect "the cheap pass does NOT see a size change" "$(n "$LAST")" "0"
u --deep
q "size:>1000 file:"
expect "the deep pass does" "$(n "$LAST")" \
    "$(find "$INC" -type f -size +1000c | wc -l)"
inc_sync "size change (deep)"

# 10. and the same for mtime, which is what dm: is answered from. The count
#     before the change is the control: it has to be exactly what the deep pass
#     later reproduces from find(1).
q "dm:today file:"
TODAY_BEFORE=$(n "$LAST")
touch "$INC/renamed.dat"
u
q "dm:today file:"
expect "the cheap pass does NOT see an mtime change" "$(n "$LAST")" "$TODAY_BEFORE"
u --deep
q "dm:today file:"
expect "the deep pass does" "$(n "$LAST")" \
    "$(find "$INC" -type f -newermt 'today 00:00' | wc -l)"
inc_sync "mtime change (deep)"

# 11. child-count: is derived from the children vector, so a removal has to take
#     the row out of it or this reports the pre-refresh number forever. The
#     expectation is counted from find(1), because a directory's link count is
#     not its child count.
q "child-count:>0 folder:"
expect "child-count: reflects the removals" "$(n "$LAST")" \
    "$(find "$INC" -type d -exec sh -c 'ls -A "$1" | head -1' _ {} \; | wc -l)"
q "parent:$INC/a folder:"
expect "a/ holds exactly its two remaining children" "$(n "$LAST")" "2"

# 12. a refresh against a tree that is not this index must refuse rather than
#     delete every row it cannot find
if "$BIN" update "$INC_DB" "$TREE" >/dev/null 2>"$TMP/err"; then
    bad "update refuses a root that is not this index"
else
    grep -q 'is not this index' "$TMP/err" \
        && ok "update refuses a root that is not this index" \
        || bad "update refuses a root that is not this index" "$(cat "$TMP/err")"
fi
inc_sync "after the refused update"

# 13. the answers survive the snapshot round trip, tombstones and all
cp "$INC_DB" "$TMP/inc-copy.idx"
if u; then ok "a second refresh is still a no-op"; else bad "second refresh"; fi
q ""
cp "$INC_DB" "$TMP/inc-copy.idx"
DB="$TMP/inc-copy.idx"
q ""
expect "the reloaded snapshot holds the same rows" "$(n "$LAST")" "$(inc_find)"
q "ext:txt"
expect "and the same extension bitmap" "$(n "$LAST")" \
    "$(find "$INC" -type f -name '*.txt' | wc -l)"
DB="$INC_DB"

# 14. an unbuilt index is not refreshable
if "$BIN" update "$TMP/junk.idx" >/dev/null 2>&1; then
    bad "update rejects a non-snapshot"
else
    ok "update rejects a non-snapshot"
fi

# 15. many directories at the same depth. The reconcile keeps one claim table per
#     depth and reuses the slots, so a directory whose table is not fully reset
#     makes its own children look new -- a duplicate row each, and no removal at
#     all. The fixture above is small enough that the slots never collide, which
#     is exactly why this needed a tree rather than another assertion.
say "incremental refresh: wide tree (claim-table reuse)"
WIDE="$TMP/wide"
for i in $(seq 1 40); do
    mkdir -p "$WIDE/d$i"
    for j in 1 2 3; do printf 'w' >"$WIDE/d$i/f$j.txt"; done
done
WIDE_DB="$TMP/wide.idx"
build "$WIDE" "$WIDE_DB" >/dev/null
DB="$WIDE_DB"
if "$BIN" update "$WIDE_DB" --deep >/dev/null 2>"$TMP/err"; then
    q ""
    expect "a deep pass duplicates nothing" "$(n "$LAST")" "$(find "$WIDE" | wc -l)"
    q "parent:$WIDE/d7"
    expect "and every directory still has its three children" "$(n "$LAST")" "3"
    q "ext:txt"
    expect "with the extension bitmap intact" "$(n "$LAST")" "120"
else
    bad "deep pass over a wide tree" "see $TMP/err"
fi

# the language suite's fixture, refreshed after every one of the above, still has
# to answer the same as it did before
DB="$TREE_DB"
q "ext:conf"
expect "the original fixture is untouched by the incremental work" "$(n "$LAST")" "3"

# --------------------------------------------------------------- logging

say "instrumentation"

"$BIN" build "$TREE" -o "$TMP/probe.idx" >/dev/null 2>"$TMP/err"
grep -qE '^indexed [0-9]+ entries' "$TMP/err" \
    && ok "build prints a timing line on stderr" \
    || bad "build prints a timing line on stderr"

ESIDX_LOG=info "$BIN" query "$TREE_DB" count:1 >/dev/null 2>"$TMP/err"
grep -q '\[info \]' "$TMP/err" \
    && ok "ESIDX_LOG=info enables INFO" \
    || bad "ESIDX_LOG=info enables INFO"

ESIDX_LOG=info "$BIN" query "$TREE_DB" count:1 >/dev/null 2>"$TMP/err"
grep -q 'plan .* eval .* sort' "$TMP/err" \
    && ok "query phase timings are emitted" \
    || bad "query phase timings are emitted" "$(cat "$TMP/err")"

ESIDX_LOG=debug "$BIN" query "$TREE_DB" count:1 >/dev/null 2>"$TMP/err"
grep -q '\[debug\]' "$TMP/err" \
    && ok "ESIDX_LOG=debug enables DEBUG" \
    || bad "ESIDX_LOG=debug enables DEBUG"

ESIDX_LOG=error "$BIN" query "$TREE_DB" count:1 >/dev/null 2>"$TMP/err"
grep -qE '\[debug\]|\[info \]' "$TMP/err" \
    && bad "ESIDX_LOG=error silences INFO/DEBUG" \
    || ok "ESIDX_LOG=error silences INFO/DEBUG"

"$BIN" -v query "$TREE_DB" count:1 >/dev/null 2>"$TMP/err"
grep -q '\[debug\]' "$TMP/err" \
    && ok "-v raises the level at runtime" \
    || bad "-v raises the level at runtime"

"$BIN" --verbose=3 query "$TREE_DB" count:1 >/dev/null 2>"$TMP/err"
grep -q '\[info \]' "$TMP/err" \
    && ok "--verbose=N raises the level at runtime" \
    || bad "--verbose=N raises the level at runtime"

# --------------------------------------------------------------- summary

say "performance (visibility, not assertions)"

DB="$TMP/perf.idx"
"$BIN" build "$TEST_ROOT" -o "$DB" 2>&1 >/dev/null | sed 's/^/   /'
printf '\n== the queries the ETP client actually issues\n'
# These four shapes are what an ETP client actually puts on the wire (the trace
# the official client produced; AGENTS.md 1.4).
# Printing them next to the phase timings is the point of the section: it is the
# only place where the driver's effect is visible.
ESIDX_LOG=info "$BIN" query "$DB" "ext:conf" "sort:size:desc" "count:5" \
    2>&1 >/dev/null | grep -E 'matched|loaded in' | sed 's/^/   unfiltered   /'
ESIDX_LOG=info "$BIN" query "$DB" "parent:$TEST_ROOT" "folder:" \
    "sort:name:ascending" "count:200" \
    2>&1 >/dev/null | grep -E 'matched|loaded in' | sed 's/^/   browse:parent /'
ESIDX_LOG=info "$BIN" query "$DB" "image:" "count:50" \
    2>&1 >/dev/null | grep -E 'matched|loaded in' | sed 's/^/   category:image /'
ESIDX_LOG=info "$BIN" query "$DB" "path:$TEST_ROOT" "*.conf" "size:>1k" \
    "sort:date_modified:descending" "count:50" \
    2>&1 >/dev/null | grep -E 'matched|loaded in' | sed 's/^/   search:path+wc/'

printf '\n== driver selection (why the timings above look the way they do)\n'
ESIDX_LOG=debug "$BIN" query "$DB" "parent:$TEST_ROOT" "folder:" "count:5" \
    2>&1 >/dev/null | grep -E 'plan: driver' | sed 's/^/   /'
ESIDX_LOG=debug "$BIN" query "$DB" "size:>1k" "file:" "count:5" \
    2>&1 >/dev/null | grep -E 'plan: driver' | sed 's/^/   /'
ESIDX_LOG=debug "$BIN" query "$DB" "someword" "count:5" \
    2>&1 >/dev/null | grep -E 'plan: (driver|no index)' | sed 's/^/   /'

printf '\n== summary: %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf '\n--- diagnostics ---\n'
    cat "$DIAG"
fi
[ "$FAIL" -eq 0 ]
