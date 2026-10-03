#!/usr/bin/env bash
#
# ETP protocol acceptance suite.
#
#   ./test_etp.sh            run against a synthetic flat tree
#   ./test_etp.sh -v         echo every line sent and received
#   TEST_ROOT=/usr ./test_etp.sh   also exercise a real index
#
# Independent of test.sh on purpose. test.sh pins the *index* against find(1);
# this pins the *wire*, against the exact byte sequence an ETP client puts on the
# socket. They fail for different reasons, so keeping them apart means a failure
# names its layer.
#
# The client side is tools/etp_probe.c, which is a transcription of an ETP client's
# own parsing rules rather than a generic FTP client. "The probe understood the
# reply" is therefore exactly "a client would understand the reply", including for
# the fragile cases: the reply terminator rule, the literal `200-Query results` /
# `200 End.` markers, RESULT_COUNT being independent of every column toggle, and
# per-item column lines having to precede their FILE/FOLDER line.
#
# Where the specification comes from, and in what order to believe it:
#
#   1  the official Everything client, observed directly. AGENTS.md 1.4 has the
#      invocation; `-v 4` on the server logs every command it sends, so a rule
#      that is wrong here is a rule that is wrong on the wire, not in a comment.
#   2  voidtools' etp_server.c 1.0.2.5, cited by line. The wire-format baseline
#      (design D1), and `etp-probe 21 <script>` runs these same shapes against a
#      live instance of it, so the probe's rules are not just self-consistent.
#   3  the client's published search syntax, docs/everything-syntax.md.
#
# Acceptance criteria, each traced to its source:
#
#   1  220 welcome, single line             observed on :21 and by the probe
#   2  USER -> 230, or 331 then PASS        ditto
#   3  `EVERYTHING <sub> <param>`, bare or after SITE
#                                          the official client sends `SITE
#                                          EVERYTHING`; another sends it bare.
#                                          Both must work.
#   4  the subcommand sequence and order    trace of the official client
#   5  every subcommand but QUERY answers
#      one line starting "200 "             observed
#   6  QUERY answers exactly `200-Query results`, ` RESULT_COUNT n`, one leading
#      space per data line, ` FOLDER name` / ` FILE name`, `200 End.`
#                                          observed against both servers
#   7  RESULT_COUNT is the TOTAL match count, not the page size
#                                          observed; etp_server.c:5190
#   8  dates as FILETIME; unknown folder size as 18446744073709551615
#                                          etp_server.c:5229,5235
#   9  a new OFFSET re-slices without re-running the search
#                                          design §6.4, ref G3
#  10  QUIT followed by an immediate close, reply never read
#                                          observed on :21

set -u
cd "$(dirname "$0")"

BIN=./esidx
PROBE=./etp-probe
TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-etp.XXXXXX")
SRV_PID=""
SRV_PID2=""
cleanup() {
    [ -n "$SRV_PID" ]  && kill "$SRV_PID"  2>/dev/null
    [ -n "$SRV_PID2" ] && kill "$SRV_PID2" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

DIAG="$TMP/diag.log"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

PASS=0
FAIL=0
say()    { printf '\n== %s\n' "$*"; }
ok()     { PASS=$((PASS + 1)); printf '   \033[32mok\033[0m   %s\n' "$1"; }
bad()    { FAIL=$((FAIL + 1)); printf '   \033[31mFAIL\033[0m %s\n' "$1"
           if [ "$#" -gt 1 ]; then printf '         %s\n' "$2"; fi; }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got [$2] want [$3]"; fi; }

# ------------------------------------------------------------------ fixture
#
# Flat, so every expected count is a literal and a wrong answer names the entry it
# wrongly included or dropped. 14 entries = 6 directories (tree included) + 8 files.
#
#   dirs   tree alpha beta gamma gamma/inner empty
#   txt    a.txt  beta/two.txt  gamma/inner/three.txt
#   conf   b.conf  c.conf  alpha/one.conf
#   log    d.log
#   noext  gamma/.dotfile
#   empty  a.txt d.log gamma/.dotfile empty/

TREE="$TMP/tree"
export TREE
mkdir -p "$TREE/alpha" "$TREE/beta" "$TREE/gamma/inner" "$TREE/empty"
: >"$TREE/a.txt"
printf 'x%.0s' $(seq 1 500)  >"$TREE/b.conf"
printf 'x%.0s' $(seq 1 5000) >"$TREE/c.conf"
: >"$TREE/d.log"
printf 'y%.0s' $(seq 1 200)  >"$TREE/alpha/one.conf"
printf 'z%.0s' $(seq 1 100)  >"$TREE/beta/two.txt"
printf 'q'                   >"$TREE/gamma/inner/three.txt"
: >"$TREE/gamma/.dotfile"
N_TREE=$(find "$TREE" | wc -l)
TREE_NAME=$(basename "$TREE")
# the spelling the PATH column must use: the client joins `path + "\" + name`
TREE_BS=${TREE//\//\\}
# The indexed root's own parent. It is a directory that exists but is *not* in the
# index, so it is what the root row's PATH column has to be -- and its name is a word
# that occurs in the root's path and in no entry's name, which is what makes the two
# assertions in 3b able to fail.
PARENT_DIR=${TREE%/*}
PARENT_NAME=$(basename "$PARENT_DIR")
PARENT_BS=${PARENT_DIR//\//\\}

echo "   fixture: $N_TREE entries under $TREE"

# ------------------------------------------------------------------- build

DB="$TMP/tree.idx"
if ! "$BIN" build "$TREE" -o "$DB" 2>"$TMP/build.log"; then
    echo "cannot build the fixture index"; sed 's/^/   /' "$TMP/build.log"; exit 1
fi
grep -E '^indexed ' "$TMP/build.log" | sed 's/^/   /'
cat "$TMP/build.log" >>"$DIAG"

if [ ! -x "$PROBE" ]; then
    echo "$PROBE not built; run 'make etp-probe'" >&2
    exit 1
fi

# ------------------------------------------------------------------ server

# start_server <tag> [extra args...] -> sets SRV_PORT_<tag>, SRV_PID_<tag>
start_server() {
    local tag="$1"; shift
    local out="$TMP/srv-$tag.out" err="$TMP/srv-$tag.err"
    # -v 3 so INFO lines (cache hits, query timings) are visible: the suite
    # asserts on some of them, and a silent log would make that vacuous
    "$BIN" -v 3 serve "$DB" -p 0 --bind 127.0.0.1 "$@" >"$out" 2>"$err" &
    local pid=$!
    local port=""
    for _ in $(seq 1 100); do
        port=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$err" 2>/dev/null | head -1)
        [ -n "$port" ] && break
        sleep 0.05
    done
    if [ -z "$port" ]; then
        echo "server '$tag' did not report a port"; sed 's/^/   /' "$err"; exit 1
    fi
    case "$tag" in
        main) SRV_PID=$pid;  SRV_PORT=$port;  SRV_ERR=$err ;;
        auth) SRV_PID2=$pid; SRV_PORT2=$port; SRV2_ERR=$err ;;
        *) echo "unknown server tag '$tag'"; exit 1 ;;
    esac
    echo "   server '$tag': pid $pid on 127.0.0.1:$port"
}
start_server main

# ------------------------------------------------------------------- probe
#
# etp <name> <port> [script on stdin]
#
# Reads the probe's output into $OUT and asserts that it contains no
# PROTOCOL-ERROR line, i.e. that the client would not have hung or mis-parsed.
etp() {
    local name="$1" port="$2"
    local script="$TMP/script"
    cat >"$script"
    if [ "$VERBOSE" = 1 ]; then
        sed 's/^/     | /' "$script"
    fi
    "$PROBE" "$port" "$script" >"$OUT" 2>"$TMP/probe.err"
    local rc=$?
    if [ $rc -ne 0 ]; then
        bad "$name" "$(grep -E 'PROTOCOL-ERROR' "$OUT" | head -3)$(cat "$TMP/probe.err")"
        cat "$OUT" >>"$DIAG"
        return 1
    fi
    ok "$name"
    cat "$OUT" >>"$DIAG"
    return 0
}
OUT="$TMP/out"

# Read values out of the probe output. When a script read more than one query
# block, only the LAST one is considered -- that is what lets a single connection
# run two QUERYs and have each be asserted on separately.
lastblock() { awk '/^BLOCK-BEGIN/ { buf = "" } { buf = buf $0 "\n" } END { printf "%s", buf }' "$OUT"; }
pcount()   { lastblock | sed -nE 's/^BLOCK-END [0-9]+ count=([0-9]+).*$/\1/p'; }
prows()    { lastblock | sed -nE 's/^BLOCK-END [0-9]+ count=[0-9]+ rows=([0-9]+).*$/\1/p'; }
# the nth row's field:  pfield <n> <key>
pfield()   { lastblock | sed -nE "s/^ROW $1 [A-Z]+ [^ ]* .*$2=([^ ]*).*\$/\1/p" | head -1; }
# every row's name, in order
pnames()   { lastblock | sed -nE 's/^ROW [0-9]+ [A-Z]+ ([^ ]*).*$/\1/p' | tr '\n' ' '; }
# the wire lines, so a raw-format assertion can be made without the probe
pwire(){ grep -E '^REPLY ' "$OUT" | sed 's/^REPLY //'; }

# ------------------------------------------------------------------- 1: login

say "1. welcome and login (criteria 1, 2)"

# The probe reads the welcome itself and fails the whole run unless it is a single
# line starting "220 " (criterion 1), so the first exchange below getting that far
# is already the assertion. The line it saw is echoed for the record.
if "$PROBE" "$SRV_PORT" /dev/null >"$TMP/w" 2>&1 && grep -q '^WELCOME 220 ' "$TMP/w"; then
    ok "220 welcome on a single line (criterion 1)"
else
    bad "220 welcome on a single line" "$(head -2 "$TMP/w")"
fi
cat "$TMP/w" >>"$DIAG"

etp "USER logs in when no password is configured" "$SRV_PORT" <<'EOF'
send USER anonymous
EOF
expect "  USER is answered 230" "$(pwire | tail -1 | cut -c1-3)" "230"

etp "USER then PASS, the two-step form" "$SRV_PORT" <<'EOF'
send USER someone
send PASS secret
EOF

etp "QUIT then an immediate close, reply unread (criterion 10)" "$SRV_PORT" <<'EOF'
send USER anonymous
sendraw QUIT
close
EOF

say "1b. password authentication"

start_server auth -u etpuser -w s3cret

etp "USER -> 331, wrong PASS -> 530" "$SRV_PORT2" <<'EOF'
send USER etpuser
send PASS wrong
EOF
expect "  331 then 530" "$(pwire | tail -2 | cut -c1-3 | tr '\n' ' ' | sed 's/ $//')" "331 530"

etp "the right PASS logs in" "$SRV_PORT2" <<'EOF'
send USER etpuser
send PASS s3cret
send EVERYTHING COUNT 1
EOF
expect "  331 then 230 then 200" "$(pwire | tail -3 | cut -c1-3 | tr '\n' ' ' | sed 's/ $//')" "331 230 200"

etp "a subcommand before login is 530" "$SRV_PORT2" <<'EOF'
send EVERYTHING COUNT 1
EOF
expect "  530 Not logged on" "$(pwire | tail -1 | cut -c1-3)" "530"

kill "$SRV_PID2" 2>/dev/null; SRV_PID2=""

# ------------------------------------------- 2: the client's exact query sequence

say "2. the ETP client's query sequence, verbatim (criteria 3, 4, 5, 6)"
echo "   the sequence below is the trace the official client produced: CASE PATH"
echo "   REGEX WHOLE_WORD, then all seven *_COLUMN toggles, then SORT, OFFSET,"
echo "   COUNT, SEARCH, QUERY."

etp "search: full sequence, 3 .conf files" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING CASE 0
send EVERYTHING PATH 0
send EVERYTHING REGEX 0
send EVERYTHING WHOLE_WORD 0
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING ATTRIBUTES_COLUMN 1
send EVERYTHING DATE_MODIFIED_COLUMN 1
send EVERYTHING DATE_CREATED_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING FILE_LIST_FILENAME_COLUMN 0
send EVERYTHING DATE_RECENTLY_CHANGED_COLUMN 0
send EVERYTHING SORT name_ascending
send EVERYTHING OFFSET 0
send EVERYTHING COUNT 50
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
EOF

expect "  every subcommand acked with a single-line 200" \
    "$(pwire | grep -c '^200 ')" "15"
expect "  no subcommand answered with a continuation line" \
    "$(pwire | grep -c '^200-')" "0"
expect "  RESULT_COUNT is the total match count" "$(pcount)" "3"
expect "  three rows returned"                   "$(prows)" "3"
expect "  name_ascending order -- the wire carries the bare name" \
    "$(pnames)" "b.conf c.conf one.conf "
echo "     and the client joins it onto PATH itself,"
echo "     which is why PATH must round-trip through the parent's own spelling"

say "3. column lines precede their row, and are complete (criteria 6, 8)"

# The client resets its per-item accumulators on FILE/FOLDER, so a column line
# that arrives after its row is credited to the *next* row. A server that emitted
# them out of order would silently show the wrong size against the wrong name.
expect "  every row has a PATH"        "$(grep -cE 'path=.+'   "$OUT")" "$(prows)"
expect "  every row has a SIZE"        "$(grep -c 'size=[0-9-]' "$OUT")" "$(prows)"
expect "  every row has ATTRIBUTES"    "$(grep -c 'attrib=[0-9-]' "$OUT")" "$(prows)"
expect "  every row has DATE_MODIFIED" "$(grep -c 'dm=[0-9-]' "$OUT")" "$(prows)"

ft=$(pfield 0 dm)
ms=$(( ft / 10000 - 11644473600000 ))
yr=$(( ms / 1000 / 60 / 60 / 24 / 365 + 1970 ))
if [ "$ft" -gt 0 ] && [ "$yr" -ge 2020 ] && [ "$yr" -le 2100 ]; then
    ok "  DATE_MODIFIED $ft decodes to year $yr (FILETIME, criterion 8)"
else
    bad "  DATE_MODIFIED decodes to a sane year" "got $yr from '$ft'"
fi

expect "  PATH is the parent directory, backslash-separated" \
    "$(pfield 0 path)" "$TREE_BS"

say "3b. the indexed root is a row like any other: basename, and a parent path"

# voidtools' server is the specification, and it prints two shapes (etp-probe 21):
#
#   ROW 0    FOLDER C:        path=
#   ROW 854  FOLDER ShareToPC  path=C:\Users\linswin\AndroidStudioProjects
#
# One rule produces both: the name is the last component of the entry's own path and
# PATH is everything before the last separator in it. Neither shape mentions a root.
# We stored the root's name as the absolute path it was indexed from -- which
# path_of() and di_lookup() both need -- and printed that whole path as the name with
# an empty PATH, so `name:` also matched the parts of the path *above* the root.
# Nothing caught it: every other row's parent is in the index, so the two spellings
# only ever differ on this one row, and no term in test.sh or cmp_ref.sh read it.

etp "the root row: name is the basename, PATH is the directory above it" \
    "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING PATH_COLUMN 1
send EVERYTHING COUNT 10
send EVERYTHING SEARCH name:$TREE_NAME folder:
sendraw EVERYTHING QUERY
query
EOF
expect "  exactly one row, and it is the indexed root" "$(prows)" "1"
expect "  its name is the basename, not the path"      "$(pnames)" "$TREE_NAME "
expect "  its PATH is the directory containing it"     "$(pfield 0 path)" "$PARENT_BS"

etp "a name: term naming the parent matches nothing" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING COUNT 10
send EVERYTHING SEARCH name:$PARENT_NAME
sendraw EVERYTHING QUERY
query
EOF
expect "  no entry is named after the root's parent" "$(pcount)" "0"

say "4. the browse sequence the client uses for a directory listing"

etp "browse: parent: + folder:, size/path/mtime/attributes columns" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING CASE 0
send EVERYTHING PATH 0
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING ATTRIBUTES_COLUMN 1
send EVERYTHING DATE_MODIFIED_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING SORT name_ascending
send EVERYTHING OFFSET 0
send EVERYTHING COUNT 200
send EVERYTHING SEARCH parent:"$TREE" folder:
sendraw EVERYTHING QUERY
query
EOF

expect "  four subdirectories"      "$(prows)" "4"
# Everything sends 18446744073709551615 on the wire for a folder whose size it
# does not index; that overflows a client's signed 64-bit field, which it
# swallows, so the client ends up showing no size at all. Both halves are
# asserted: the wire bytes, and the value the client is left holding.
expect "  the wire sends the unknown-size sentinel" \
    "$(lastblock | grep -c '^WIRE  SIZE 18446744073709551615$')" "4"
expect "  the client is left with no size for a folder" "$(pfield 0 size)" "-1"
expect "  a folder's ATTRIBUTES has DIRECTORY set" \
    "$(pfield 0 attrib | awk '{ print ($1 % 32 >= 16) ? "dir" : "notdir" }')" "dir"

# the same directory, files only -- the client issues this as a second round trip
etp "browse: parent: + !folder:" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 200
send EVERYTHING SEARCH parent:"$TREE" !folder:
sendraw EVERYTHING QUERY
query
EOF
expect "  four files at the top level"  "$(prows)" "4"

say "5. paging re-slices without re-running the search (criterion 9)"

etp "page 1" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 1
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
EOF
P1=$(pnames)
expect "  one row"            "$(prows)" "1"
expect "  total is 3"         "$(pcount)" "3"

etp "page 2 at OFFSET 1" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING OFFSET 1
send EVERYTHING COUNT 1
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
EOF
expect "  one row"            "$(prows)" "1"
expect "  total still 3"      "$(pcount)" "3"
if [ "$P1" != "$(pnames)" ]; then
    ok "  OFFSET 1 returned a different row than OFFSET 0"
else
    bad "  OFFSET 1 returned a different row" "both pages: $P1"
fi

# A repeated QUERY on one connection with identical parameters must hit the cache.
etp "a repeated QUERY on one connection" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 2
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
sendraw EVERYTHING QUERY
query
EOF
expect "  the repeat returns the same rows" \
    "$(pnames)" "b.conf c.conf "
if grep -q 'cache hit' "$SRV_ERR"; then
    ok "  the result cache served the repeat (design §6.4, ref G3)"
else
    bad "  the result cache served the repeat" "no 'cache hit' in the server log"
fi

say "6. all 32 subcommands (criteria 3, 5)"

sub() {  # sub <NAME> <PARAM> <expected regex>
    local n="$1" p="$2" want="$3"
    if etp "EVERYTHING $n $p" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING $n $p
EOF
    then
        if pwire | tail -1 | grep -qE "$want"; then
            ok "  reply: $(pwire | tail -1)"
        else
            bad "  reply to $n $p" "got [$(pwire | tail -1)] want /$want/"
        fi
    fi
}

sub CASE 1 '^200 Case set to \(1\)\.$'
sub WHOLE_WORD 1 '^200 Whole word set to \(1\)\.$'
sub PATH 1 '^200 Path set to \(1\)\.$'
sub DIACRITICS 1 '^200 Diacritics set to \(1\)\.$'
sub PREFIX 1 '^200 Prefix set to \(1\)\.$'
sub SUFFIX 1 '^200 Suffix set to \(1\)\.$'
sub IGNORE_PUNCTUATION 1 '^200 Ignore punctuation set to \(1\)\.$'
sub IGNORE_WHITESPACE 1 '^200 Ignore whitespace set to \(1\)\.$'
sub REGEX 1 '^200 Regex set to \(1\)\.$'
sub HIDE_EMPTY_SEARCH_RESULTS 1 '^200 Hide empty search results set to \(1\)\.$'
sub SEARCH hello '^200 Search set to \(hello\)\.$'
sub FILTER_SEARCH abc '^200 Filter search set to \(abc\)\.$'
sub FILTER_CASE 1 '^200 Filter case set to \(1\)\.$'
sub FILTER_DIACRITICS 1 '^200 Filter diacritics set to \(1\)\.$'
sub FILTER_PREFIX 1 '^200 Filter prefix set to \(1\)\.$'
sub FILTER_SUFFIX 1 '^200 Filter suffix set to \(1\)\.$'
sub FILTER_IGNORE_PUNCTUATION 1 '^200 Filter ignore punctuation set to \(1\)\.$'
sub FILTER_IGNORE_WHITESPACE 1 '^200 Filter ignore whitespace set to \(1\)\.$'
sub FILTER_PATH 1 '^200 Filter path set to \(1\)\.$'
sub FILTER_REGEX 1 '^200 Filter regex set to \(1\)\.$'
sub FILTER_WHOLE_WORD 1 '^200 Filter whole word set to \(1\)\.$'
sub SIZE_COLUMN 1 '^200 Size column set to \(1\)\.$'
sub ATTRIBUTES_COLUMN 1 '^200 Attributes column set to \(1\)\.$'
sub DATE_MODIFIED_COLUMN 1 '^200 Date modified column set to \(1\)\.$'
sub DATE_CREATED_COLUMN 1 '^200 Date created column set to \(1\)\.$'
sub PATH_COLUMN 1 '^200 Path column set to \(1\)\.$'
sub FILE_LIST_FILENAME_COLUMN 1 '^200 File list filename column set to \(1\)\.$'
sub DATE_RECENTLY_CHANGED_COLUMN 1 '^200 Date recently changed column set to \(1\)\.$'

say "6b. all 22 sort names (design §1.2)"

for s in name path size extension date_created date_modified attributes \
         file_list_filename date_recently_changed; do
    for d in ascending descending; do
        if etp "SORT ${s}_${d}" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING SORT ${s}_${d}
EOF
        then :; fi
    done
done
for d in ascending descending; do
    if etp "SORT inverse_size_${d}" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING SORT inverse_size_${d}
EOF
    then :; fi
done

etp "an unknown sort name is 500 Unknown sort type." "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING SORT nosuchthing_ascending
EOF
expect "  500 Unknown sort type." "$(pwire | tail -1)" "500 Unknown sort type."

etp "an unknown subcommand is 500 Unknown Everything command." "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING NOSUCHTHING
EOF
expect "  500 Unknown Everything command." "$(pwire | tail -1)" "500 Unknown Everything command."

say "7. SITE EVERYTHING is accepted identically (criterion 3)"

etp "SITE form, same rows as the bare form" "$SRV_PORT" <<'EOF'
send USER anonymous
send SITE EVERYTHING CASE 1
send SITE EVERYTHING SIZE_COLUMN 1
send SITE EVERYTHING PATH_COLUMN 1
send SITE EVERYTHING SORT size_descending
send SITE EVERYTHING COUNT 2
send SITE EVERYTHING SEARCH ext:conf
sendraw SITE EVERYTHING QUERY
query
EOF
expect "  two rows"                "$(prows)" "2"
expect "  size_descending order"   "$(pnames)" "c.conf b.conf "

say "8. the filter group is a second, independent stage (design §1.1, ref F8)"

etp "FILTER_SEARCH narrows the primary result set" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 10
send EVERYTHING SEARCH ext:conf
send EVERYTHING FILTER_SEARCH one
sendraw EVERYTHING QUERY
query
EOF
expect "  3 rows narrowed to 1"           "$(prows)" "1"
expect "  RESULT_COUNT reflects the filter" "$(pcount)" "1"
expect "  the surviving row is one.conf"   "$(pnames)" "one.conf "

etp "clearing the filter restores the full set" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 10
send EVERYTHING SEARCH ext:conf
send EVERYTHING FILTER_SEARCH one
send EVERYTHING FILTER_SEARCH
sendraw EVERYTHING QUERY
query
EOF
expect "  clearing the filter restores the full set" "$(prows)" "3"

say "9. empty and nonsensical searches"

etp "an empty SEARCH matches the whole tree" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING COUNT 3
send EVERYTHING SEARCH 
sendraw EVERYTHING QUERY
query
EOF
expect "  COUNT caps the page"          "$(prows)" "3"
expect "  RESULT_COUNT is the whole tree" "$(pcount)" "$N_TREE"

etp "a search matching nothing still returns a well-formed block" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 10
send EVERYTHING SEARCH zzzznomatchatall
sendraw EVERYTHING QUERY
query
EOF
expect "  no rows"  "$(prows)" "0"
expect "  count 0"  "$(pcount)" "0"

etp "an OFFSET past the end keeps the total" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 10
send EVERYTHING OFFSET 9999
send EVERYTHING SEARCH ext:conf
sendraw EVERYTHING QUERY
query
EOF
expect "  no rows"     "$(prows)" "0"
expect "  total is 3"  "$(pcount)" "3"

etp "a syntactically broken search does not break the block" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 10
send EVERYTHING SEARCH <unclosed
sendraw EVERYTHING QUERY
query
EOF
expect "  no rows, but the block is intact" "$(prows)" "0"

say "10. the search strings the client builds, end to end"

# The strings below are the ones EtpBrowseViewModel / EtpSearchViewModel /
# EtpExplorerViewModel assemble, with the fixture paths substituted. The expected
# counts are what find(1) predicts for this tree.
cq() {  # cq <desc> <search> <want-count>
    if etp "$1" "$SRV_PORT" <<EOF
send USER anonymous
send EVERYTHING CASE 0
send EVERYTHING PATH 0
send EVERYTHING REGEX 0
send EVERYTHING WHOLE_WORD 0
send EVERYTHING SIZE_COLUMN 1
send EVERYTHING ATTRIBUTES_COLUMN 1
send EVERYTHING DATE_MODIFIED_COLUMN 1
send EVERYTHING DATE_CREATED_COLUMN 1
send EVERYTHING PATH_COLUMN 1
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 200
send EVERYTHING SEARCH $2
sendraw EVERYTHING QUERY
query
EOF
    then
        expect "  $1 -> $3 results" "$(pcount)" "$3"
    fi
}

cq "browse: parent: + folder:"     "parent:\"$TREE\" folder:"       "4"
cq "browse: parent: + !folder:"    "parent:\"$TREE\" !folder:"      "4"
cq "browse: a subdirectory"        "parent:\"$TREE/gamma\" folder:" "1"
cq "ext: OR chain"                 "ext:conf | ext:log"            "4"
cq "ext: multi-value list"         "ext:conf;txt;log"              "7"
cq "type: category with no members" "image:"                        "0"
cq "size: >1k -- a 4096-byte directory passes too" "size:>1k"      "7"
cq "size: a..b"                    "size:100..1000"                 "3"
cq "attrib: the dot file"          "attrib:h"                       "1"
cq "empty:"                        "empty:"                         "4"
cq "path:regex: with a backslash anchor, as the client sends it" \
   'path:regex:gamma\\inner$' "1"
cq "path: + wildcard"              "path:alpha *.conf"             "1"
cq "dm: today"                     "dm:today"                       "14"
cq "a bare word as a substring"    "conf"                           "3"

say "11. FTP verbs the client never sends, for other clients"

# OPTS is the first command after login for the official Everything client, and it
# arrived by running that client against this server: `Everything.exe -instance X
# -connect u:p@host:port` puts `OPTS UTF8 ON` on the wire before any column
# toggle, and the server used to answer 501 because it compared the whole argument
# against the bare word "UTF8". The client then never issued the toggles or QUERY
# at all, so the client sat there with no IPC reply and no error anywhere -- a
# silent hang, which is why no suite caught it: nothing here drives the official
# client, and the probe only ever sent what it had been taught to send.
# Reply wording and the bare-argument rejection are the reference's, checked
# against etp_server 1.0.2.5 listening on 127.0.0.1:21.
etp "OPTS UTF8 ON/OFF, the official client's first command after login" "$SRV_PORT" <<'EOF'
send USER anonymous
send OPTS UTF8 ON
send OPTS UTF8 OFF
send OPTS UTF8
send OPTS MLST
EOF
expect "  OPTS UTF8 ON is accepted, not rejected" \
    "$(pwire | sed -n 2p | cut -c1-3)" "200"
expect "  ...with the reference's wording" \
    "$(pwire | sed -n 2p)" "200 UTF8 mode enabled."
expect "  OPTS UTF8 OFF disables it again" \
    "$(pwire | sed -n 3p)" "200 UTF8 mode disabled."
expect "  the bare option is rejected, as the reference rejects it" \
    "$(pwire | sed -n 4p | cut -c1-3)" "501"
expect "  an unrelated option is rejected" \
    "$(pwire | sed -n 5p | cut -c1-3)" "501"

# FEAT deserves its own note. The server emits exactly the reference feature set
# (etp_server.c:1728-1736), and a client's own reply-reading rule then mis-parses
# it: " MLSD" is five characters with 'S' at index 3, so it matches neither the
# final-line test (space at index 3) nor the continuation test (hyphen at index 3)
# and the reader stops there. That is the same rule the probe implements, which is
# why the probe truncates the reference's reply at exactly the same line.
#
# A client therefore never sends FEAT -- it does not need to probe for an
# extension it always uses. The server is not at fault and must not be "fixed"
# around it; what is asserted here is that the truncation happens exactly where
# the rule says it does, so a future reader is not surprised.
etp "FEAT: the client's parser truncates the reference feature list" "$SRV_PORT" <<'EOF'
send USER anonymous
send FEAT
EOF
expect "  the reply starts with the 211- continuation marker" \
    "$(pwire | sed -n 2p)" "211-Features:"
expect "  the client stops at ' MDTM', the first line it cannot classify" \
    "$(pwire | sed -n 3p)" " MDTM"
expect "  the lines after it are never delivered to the client" \
    "$(pwire | grep -c 'REST STREAM\|MLSD\|UTF8\|EVERYTHING\|211 End')" "0"
echo "   NOTE Everything's own FEAT output is truncated the same way; the client"
echo "        simply never issues FEAT. Recorded rather than worked around."

etp "NOOP, PWD and XPWD" "$SRV_PORT" <<'EOF'
send USER anonymous
send NOOP
send PWD
send XPWD
EOF
expect "  NOOP is 200"    "$(pwire | sed -n 2p | cut -c1-3)" "200"
expect "  two 257 replies" "$(pwire | grep -c '^257 ')" "2"

etp "CWD into a real directory, CDUP back out" "$SRV_PORT" <<EOF
send USER anonymous
send CWD $TREE/alpha
send PWD
send CDUP
send PWD
EOF
expect "  CWD is 250" "$(pwire | sed -n 2p | cut -c1-3)" "250"
expect "  PWD shows the new directory" "$(pwire | sed -n 3p | grep -c 'alpha')" "1"
expect "  CDUP is 250" "$(pwire | sed -n 4p | cut -c1-3)" "250"

etp "SIZE and MDTM answer for an indexed file" "$SRV_PORT" <<EOF
send USER anonymous
send SIZE $TREE/c.conf
send MDTM $TREE/c.conf
EOF
expect "  SIZE is 213 5000" "$(pwire | sed -n 2p)" "213 5000"
expect "  MDTM is a timestamp" "$(pwire | sed -n 3p | grep -cE '^213 [0-9]{14}$')" "1"

etp "MLSD over a real data connection" "$SRV_PORT" <<EOF
send USER anonymous
send EPSV
connectdata
send MLSD $TREE/alpha
data
recv
EOF
expect "  EPSV is 229" "$(pwire | sed -n 2p | cut -c1-3)" "229"
expect "  MLSD is 150" "$(pwire | sed -n 3p | cut -c1-3)" "150"
expect "  the listing came back over the data connection" \
    "$(grep -c '^DATA type=file;size=200;modify=[0-9]*; one.conf$' "$OUT")" "1"
expect "  MLSD completes with 226" "$(pwire | sed -n 4p | cut -c1-3)" "226"

etp "PASV + LIST -l over a real data connection" "$SRV_PORT" <<EOF
send USER anonymous
send PASV
connectdata
send LIST -l $TREE/alpha
data
recv
EOF
expect "  PASV is 227" "$(pwire | sed -n 2p | cut -c1-3)" "227"
expect "  LIST is 150" "$(pwire | sed -n 3p | cut -c1-3)" "150"
expect "  the ls-style line carries the mode, size, date and name" \
    "$(grep -cE '^DATA -rw-r--r-- +[0-9]+ +esidx +esidx +200 +[A-Z][a-z]{2} +[0-9]+ +[0-9:]+ +one\.conf$' "$OUT")" "1"
expect "  LIST completes with 226" "$(pwire | sed -n 4p | cut -c1-3)" "226"

# MLSD of the scan root goes through the INDEX path, so it reports what the index
# holds -- four subdirectories and four top-level files. The root row itself has no
# parent row, so it is not listed, which is the same answer the directory has.
etp "MLSD of an indexed directory reads the index" "$SRV_PORT" <<EOF
send USER anonymous
send EPSV
connectdata
send MLSD $TREE
data
recv
EOF
expect "  the four subdirectories are listed" \
    "$(grep -c '^DATA type=dir;size=' "$OUT")" "4"
expect "  the four top-level files are listed" \
    "$(grep -c '^DATA type=file;size=' "$OUT")" "4"

# A path the index has never seen has to fall back to the real filesystem, or a
# stock FTP client breaks the moment it walks outside the indexed tree.
etp "MLSD of an unindexed path falls back to the filesystem" "$SRV_PORT" <<EOF
send USER anonymous
send EPSV
connectdata
send MLSD $TMP
data
recv
EOF
expect "  the unindexed directory is listed too" \
    "$(grep -c '^DATA type=dir;size=.*; tree$' "$OUT")" "1"

# The reference answers 503 while a transfer is in flight
# (etp_server.c:1707-1712), but only once the data connection is actually open --
# PASV merely arms a listener. Since a transfer runs to completion inside one
# command here, a client can never observe that state, so there is nothing to
# assert. Asserting it would be asserting an unreachable branch.

etp "an unknown FTP verb is 500 Unknown command." "$SRV_PORT" <<'EOF'
send USER anonymous
send FROBNICATE
EOF
expect "  500 Unknown command." "$(pwire | tail -1)" "500 Unknown command."

say "12. nothing in the exchange was unparseable"

# The probe exits non-zero on any PROTOCOL-ERROR, and every `etp` above would have
# failed. This is the belt-and-braces version: count them across the whole log.
if grep -q 'PROTOCOL-ERROR' "$DIAG"; then
    bad "no reply was unparseable by the client's own rules" \
        "$(grep -m3 'PROTOCOL-ERROR' "$DIAG")"
else
    ok "no reply was unparseable by the client's own rules"
fi

if grep -q 'DROPPED' "$DIAG"; then
    bad "no line was silently dropped by the client's parser" \
        "$(grep -m3 'DROPPED' "$DIAG")"
else
    ok "no line was silently dropped by the client's parser"
fi

# ------------------------------------------------------------------ summary

printf '\n== server log (tail)\n'
tail -20 "$SRV_ERR" | sed 's/^/   /'

printf '\n== summary: %d passed, %d failed\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf '\n--- diagnostics ---\n'
    tail -80 "$DIAG"
fi
[ "$FAIL" -eq 0 ]
