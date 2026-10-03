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
q "regex:^b.*conf\$"         ; expect "regex:"                    "$(n "$LAST")" "1"
q 'path:regex:sub1/deep$'    ; expect "modifiers stack in the value" "$(n "$LAST")" "1"
q 'path:regex:[a-z]\.conf$'  ; expect "regex classes and escapes"  "$(n "$LAST")" "3"
q "case:regex:^B"            ; expect "case: is case SENSITIVE"    "$(n "$LAST")" "0"
q "nocase:regex:^B"          ; expect "nocase: overrides the default" "$(n "$LAST")" "1"
q "ww:conf"                  ; expect "ww:"                       "$(n "$LAST")" "3"
q "child:b.conf"             ; expect "child:<expr>"              "$(n "$LAST")" "1"

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
