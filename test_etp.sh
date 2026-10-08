#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
#
# ETP protocol acceptance suite.
#
#   ./test_etp.sh            run against a synthetic flat tree
#   ./test_etp.sh -v         echo every line sent and received
#   TEST_ROOT=/usr ./test_etp.sh   also exercise a real index
#   ESIDX_BUILD=dbg ./test_etp.sh  run against the sanitiser build (what `make check` does)
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

# ESIDX_BUILD picks the flavour `make` built -- `opt` (default) or `dbg`, the -O0 +
# AddressSanitizer/UBSan one. Both are built by one `make` and their objects are named
# apart, so neither can be stale relative to the other; that is why the selection is a
# name here and not a `make DEBUG=1` that rewrites the same object files.
case "${ESIDX_BUILD:-opt}" in
    dbg) BIN=${ESIDX_BIN:-./esidx-dbg}; PROBE=./etp-probe-dbg; ORDER_REF=./order-ref-dbg ;;
    opt) BIN=${ESIDX_BIN:-./esidx};    PROBE=./etp-probe;    ORDER_REF=./order-ref ;;
    *)   printf 'ESIDX_BUILD must be opt or dbg, not "%s"\n' "${ESIDX_BUILD}" >&2; exit 2 ;;
esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-etp.XXXXXX")
SRV_PID=""
SRV_PID2=""
SRV_PID3=""
SRV_PID4=""
SRV_PID5=""
SRV_PID6=""
cleanup() {
    for p in "$SRV_PID" "$SRV_PID2" "$SRV_PID3" "$SRV_PID4" "$SRV_PID5" "$SRV_PID6"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
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
# Pin the mode: the LIST assertion below checks the mode string, and the ambient
# umask is not ours to assume -- a user with umask 002 (the Ubuntu default) would
# create -rw-rw-r-- and the test would fail on a correct server.
chmod 644 "$TREE/alpha/one.conf"
printf 'z%.0s' $(seq 1 100)  >"$TREE/beta/two.txt"
printf 'q'                   >"$TREE/gamma/inner/three.txt"
: >"$TREE/gamma/.dotfile"
N_TREE=$(find "$TREE" | wc -l)
TREE_NAME=$(basename "$TREE")
# The PATH column's spelling: POSIX, like the tree and like the index. The reference spells
# paths Windows-style because it indexes NTFS; this is the server and the tree is POSIX, so the
# wire is too -- a deliberate deviation, AGENTS.md §5.2. The assertions below are what pins it.
TREE_WIRE=$TREE
# The indexed root's own parent. It is a directory that exists but is *not* in the
# index, so it is what the root row's PATH column has to be -- and its name is a word
# that occurs in the root's path and in no entry's name, which is what makes the two
# assertions in 3b able to fail.
PARENT_DIR=${TREE%/*}
PARENT_NAME=$(basename "$PARENT_DIR")
PARENT_WIRE=$PARENT_DIR

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

# start_server <tag> [extra args...] -> sets SRV_PORT_<tag>, SRV_ERR_<tag>
#
# SRV_DB overrides the snapshot to serve, so a test can point a server at its own
# fixture: the counts asserted above are literals a reader can check by hand, and a
# test that invalidated them by mutating their tree would be worse than no test
# (design §3.2 -- the deep fixture belongs to find(1) cross-checking, not here).
start_server() {
    local tag="$1"; shift
    local out="$TMP/srv-$tag.out" err="$TMP/srv-$tag.err"
    # -v 3 so INFO lines (cache hits, query timings) are visible: the suite
    # asserts on some of them, and a silent log would make that vacuous
    "$BIN" -v 3 serve "${SRV_DB:-$DB}" -p 0 --bind 127.0.0.1 "$@" >"$out" 2>"$err" &
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
        main)     SRV_PID=$pid;  SRV_PORT=$port;  SRV_ERR=$err ;;
        auth)     SRV_PID2=$pid; SRV_PORT2=$port; SRV2_ERR=$err ;;
        refresha) SRV_PID3=$pid; SRV_PORTA=$port; SRVA_ERR=$err ;;
        refreshb) SRV_PID4=$pid; SRV_PORTB=$port; SRVB_ERR=$err ;;
        overlay)  SRV_PID5=$pid; SRV_PORTO=$port; SRVO_ERR=$err ;;
        stall)    SRV_PID6=$pid; SRV_PORTS=$port; SRVS_ERR=$err ;;
        *) echo "unknown server tag '$tag'"; exit 1 ;;
esac

# Refuse to measure a binary that is older than the code: a build that fails part-way
# leaves the previous binary in place, and this suite would then pin the *wire* against
# the last build while reporting it as this one. The suite cannot know what is inside the
# binary; the mtime is the cheapest thing it can know. Same guard in all three suites, and
# deliberately not a shared helper -- they are independent files so that one missing tool
# cannot take down the other two (AGENTS.md 1.3).
for f in *.c *.h Makefile sfa/*.c sfa/*.h; do
    [ -f "$f" ] || continue
    if [ -x "$BIN" ] && [ "$f" -nt "$BIN" ]; then
        printf 'test_etp.sh: %s is newer than %s -- build did not run or did not finish\n' \
            "$f" "$BIN" >&2
        exit 1
    fi
done
    echo "   server '$tag': pid $pid on 127.0.0.1:$port"
}
start_server main

# --------------------------------------------------------- one snapshot, one server
#
# A second `serve` of the same snapshot would answer from its own copy of the index and apply
# its own events, so the two would drift apart with every change under the tree and neither
# would report it. main.c holds an flock on <db>.lock; this is the outside view of that
# decision, and it asserts the message names the pid that actually holds it -- a lock file
# whose contents nobody checks is how the message ends up wrong.
say "one snapshot, one server"
if "$BIN" -v 3 serve "$DB" -p 0 --bind 127.0.0.1 >"$TMP/dup.out" 2>"$TMP/dup.err"; then
    bad "a second serve of the same snapshot is refused" "it started anyway"
else
    if grep -q "already being served by pid $SRV_PID" "$TMP/dup.err"; then
        ok "a second serve is refused, naming the pid that holds the snapshot"
    else
        bad "a second serve is refused, naming the holder" "$(tail -1 "$TMP/dup.err")"
    fi
fi

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
    # Bounded, because the failure this suite now asserts against *is* a hang: a reply
    # line that loses its CRLF leaves the client waiting for the end of it forever, and
    # a suite that waits with it reports nothing at all. 30 s is far longer than any
    # session here needs -- the whole suite takes about that.
    timeout 30 "$PROBE" "$port" "$script" >"$OUT" 2>"$TMP/probe.err"
    local rc=$?
    if [ $rc -eq 124 ]; then
        bad "$name" "the probe timed out -- a real client hangs here too"
        return 1
    fi
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
# run two QUERYs and have each be asserted on separately. Each takes the file to
# read, so two sessions driven at once (section 11b) do not overwrite each other.
lastblock() { awk '/^BLOCK-BEGIN/ { buf = "" } { buf = buf $0 "\n" } END { printf "%s", buf }' "${1:-$OUT}"; }
pcount()   { lastblock "${1:-$OUT}" | sed -nE 's/^BLOCK-END [0-9]+ count=([0-9]+).*$/\1/p'; }
prows()    { lastblock "${1:-$OUT}" | sed -nE 's/^BLOCK-END [0-9]+ count=[0-9]+ rows=([0-9]+).*$/\1/p'; }
# the nth row's field:  pfield <n> <key>
pfield()   { lastblock | sed -nE "s/^ROW $1 [A-Z]+ [^ ]* .*$2=([^ ]*).*\$/\1/p" | head -1; }
# every row's name, in order
pnames()   { lastblock "${1:-$OUT}" | sed -nE 's/^ROW [0-9]+ [A-Z]+ ([^ ]*).*$/\1/p' | tr '\n' ' '; }
# the wire lines, so a raw-format assertion can be made without the probe
pwire(){ grep -E '^REPLY ' "${1:-$OUT}" | sed 's/^REPLY //'; }

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

# Its own copy of the snapshot, because a second server on the *same* one is refused now
# (`serve` holds an flock on <db>.lock, main.c): two servers on one snapshot would answer from
# two copies of the index and apply two sets of events. What this server is for is the password
# handshake, and the copy has the same fixture, so nothing here needs the original.
cp "$DB" "$TMP/auth.idx"
SRV_DB="$TMP/auth.idx" start_server auth -u etpuser -w s3cret

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

# ------------------------------------------- 1b: a client that stops reading
#
# The loop is single-threaded (see the structure comment at the top of etp.c), which is only safe
# while nothing in it blocks on a client. A reply used to go out with a blocking send(), so a
# client that asked for a lot and then read nothing stopped the entire server: no other client
# answered, no filesystem event was applied, no sweep ran -- and none of that was visible from
# inside, because the loop was not at poll() to notice it. Replies are now queued on a
# non-blocking socket, and a client that does not start draining loses its slot.
say "1b. a client that stops reading costs it its slot, not the loop"

# 4000 entries, and every column switched on below, because what has to happen is a reply the
# socket cannot take: a client that does not read still has a window of a few hundred KB, and a
# 14-entry fixture's answer fits in it, so nothing would ever be queued and the test would pass
# without testing anything. Measured: 400 entries queued nothing, 2000 entries with four columns
# queued 18 KB, and the kernel decides the rest.
mkdir -p "$TMP/stall/d"
for i in $(seq 1 4000); do : >"$TMP/stall/d/f$i.txt"; done
"$BIN" build "$TMP/stall" -o "$TMP/stall.idx" >/dev/null 2>&1
SRV_DB="$TMP/stall.idx" start_server stall --stall-ms=1000

# sendraw for every command and no `query`: this client reads its welcome and then nothing, so
# the reply has nowhere to go. The probe's rcvbuf is what makes that true on a small tree.
cat >"$TMP/stall.script" <<'EOF'
rcvbuf 2048
sendraw USER anonymous
sendraw EVERYTHING PATH_COLUMN 1
sendraw EVERYTHING SIZE_COLUMN 1
sendraw EVERYTHING DATE_MODIFIED_COLUMN 1
sendraw EVERYTHING ATTRIBUTES_COLUMN 1
sendraw EVERYTHING COUNT 5000
sendraw EVERYTHING SEARCH ext:txt
sendraw EVERYTHING QUERY
sleep 4000
EOF
"$PROBE" "$SRV_PORTS" "$TMP/stall.script" >"$TMP/stall.out" 2>&1 &
STALL_CLIENT=$!
sleep 1

t0=$(date +%s%N)
etp "another client, while the first is not reading" "$SRV_PORTS" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 1
send EVERYTHING SEARCH ext:txt
sendraw EVERYTHING QUERY
query
EOF
t1=$(date +%s%N)
elapsed=$(( (t1 - t0) / 1000000 ))
expect "  the other client is answered, row and all" "$(prows)" "1"
if [ "$elapsed" -lt 5000 ]; then
    ok "  and promptly (${elapsed} ms)"
else
    bad "  and promptly" "took ${elapsed} ms: the loop was inside the other client's send()"
fi

wait "$STALL_CLIENT" 2>/dev/null
expect "  the stalled client is dropped, and the log says so" \
    "$(grep -c 'no progress for' "$SRVS_ERR")" "1"

etp "and the server is still serving afterwards" "$SRV_PORTS" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 1
send EVERYTHING SEARCH ext:txt
sendraw EVERYTHING QUERY
query
EOF
expect "  the next client is answered too" "$(prows)" "1"
kill "$SRV_PID6" 2>/dev/null; SRV_PID6=""

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
echo "     and a client joins it onto PATH itself -- which is why PATH carries the parent's own"
echo "     spelling, POSIX on this side (AGENTS.md §5.2)"

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

expect "  PATH is the parent directory, POSIX-separated" \
    "$(pfield 0 path)" "$TREE_WIRE"

# Nothing on the wire is Windows-spelled. This is the assertion that would fail if a conversion
# like the old wire_path() came back, and it reads the raw lines rather than a decoded field, so
# it covers every column at once -- PATH, FILE_LIST_FILENAME, and whatever is added next.
expect "  no backslash in any wire line" "$(grep -c '\\' "$OUT")" "0"

say "3b. the indexed root is a row like any other: basename, and a parent path"

# voidtools' server is the specification, and it prints two shapes (etp-probe 21):
#
#   ROW 0    FOLDER C:        path=
#   ROW 854  FOLDER ShareToPC  path=C:\Users\linswin\AndroidStudioProjects
#
# The spelling there is the reference's, and only because it indexes NTFS: our PATH column is
# POSIX (AGENTS.md §5.2). What is being pinned below is the *rule*, which is the same on both
# sides -- the name is the last component of the entry's own path and
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
expect "  its PATH is the directory containing it"     "$(pfield 0 path)" "$PARENT_WIRE"

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

say "5b. a query the client has stopped waiting for (D12)"

# Typing into Everything sends SEARCH and QUERY per keystroke and throws the previous
# answer away, so the queue on a live connection is mostly work nobody will read -- and
# being single-threaded, the server delays the query the user *is* waiting for by all of
# it: 2 776 ms for the slowest query measured on the 8.9M-entry r7000 index.
#
# `burst` puts two whole SEARCH/QUERY pairs in ONE write, which is what that queue looks
# like on the wire. The same search both times is deliberate: it is what catches an
# abandoned query leaving an empty entry in the result cache, which the second query would
# then have served.
hits_before=$(grep -c 'cache hit' "$SRV_ERR")
etp "two queries pipelined on one connection" "$SRV_PORT" <<'EOF'
send USER anonymous
send EVERYTHING COUNT 10
burst EVERYTHING SEARCH ext:conf\nEVERYTHING QUERY\nEVERYTHING SEARCH ext:conf\nEVERYTHING QUERY
query
query
EOF
B1=$(sed -nE 's/^BLOCK-END 1 count=([0-9]+).*/\1/p' "$OUT")
B2=$(sed -nE 's/^BLOCK-END 2 count=([0-9]+).*/\1/p' "$OUT")
if [ "$B1" = "0" ]; then
    ok "  the superseded query is answered, with an empty block (count 0)"
else
    bad "  the superseded query is answered with an empty block" "block 1 count=$B1"
fi
# The block still has to be complete: a reply that stops mid-sentence is the failure the
# 1023-byte reply buffer and the OPTS UTF8 hang both were, and the probe is a client, so
# reaching the second block at all is the assertion that the first one ended.
if [ -n "$B2" ] && [ "$B2" -gt 0 ]; then
    ok "  the query the client is waiting for still runs (count $B2)"
else
    bad "  the query the client is waiting for still runs" "block 2 count=${B2:-none}"
fi
if [ "$(grep -c 'cache hit' "$SRV_ERR")" = "$hits_before" ]; then
    ok "  the abandoned query left nothing in the result cache"
else
    bad "  the abandoned query left nothing in the result cache" \
        "the second query was served from the cache"
fi
if grep -q 'abandoned' "$SRV_ERR"; then
    ok "  and the log says why -- an abandoned query is not silent"
else
    bad "  the log says why" "no 'abandoned' line in the server log"
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

say "10b. a search longer than every buffer it used to cross"

# Two fixed buffers on this path cut a long search, and neither said so.
#
# The acknowledgement was one of them. `EVERYTHING SEARCH` answers by echoing the whole
# search (etp_server.c:4027), and c_reply() formatted that into a char[1024]: past 1023
# bytes vsnprintf drops the tail *and the CRLF*, so the reply never ends and the client
# waits for the rest of a line that is not coming. No error on either side, the session
# simply goes quiet -- the same shape as the OPTS UTF8 hang in section 11, found the same
# way. The other was a 4096-byte search buffer: past that, the query answered was not the
# query asked, which is worse than a hang (design §5.3).
#
# A client can produce this: Everything's search box takes tens of thousands of
# characters and `ext:` is the shape that gets there. The reference has neither buffer --
# its search lives in a string it reallocs per SEARCH (etp_server.c:4025) and the
# acknowledgement goes straight into the client's stream -- and measured against it on
# :21, a 451-name `ext:` list answers exactly what `ext:c` alone answers. That comparison
# is a term in ./cmp_ref.sh so it can be re-run.
#
# 400 padded names is a 6 900-character value, past the 4096 search buffer and well past
# the 1023 reply buffer, with the one extension that exists *last* so that any cut along
# the way drops it and the answer comes back empty.
PAD=$(for i in $(seq 1 400); do printf 'zzzzzzzzzzzzzz%s;' "$i"; done)
cq "a 6 900-character ext: value: the ack is whole and the query runs" \
   "ext:${PAD}conf" "3"
# and the same value through the *bare word* path, which the parser sizes differently
q_name=$(printf 'z%.0s' $(seq 1 6000))
cq "a 6 000-character bare word matches nothing rather than a prefix of it" \
   "$q_name" "0"

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

say "11b. a server that reconciles its own index while it is answering (design §7)"

# Two servers, two index configurations, because one process has one configuration and
# the two defects need opposite ones:
#
#   A, everything built. A newly indexed row has to reach the three numeric sorted
#      arrays, or `size:` cannot see a file the server indexed a moment ago. esidx_add()
#      did not push them: it has nothing to link *from*, and every test passed anyway
#      because `esidx update` exits and the next process rebuilds the arrays from the
#      columns. The path only exists in a process that appends and answers questions.
#
#   B, --no-index=size. The delta must NOT reach a skipped array. update_merge() merges
#      on `dn * 100 > n`, true the moment n is 0, so a retraction would become a main
#      array of one row -- and range_on() reads "built" as `s->n != 0`, turning a
#      deliberately unbuilt index into a partially built one that answers `size:` from
#      the rows it happens to hold. keep.txt is the tell: it is never touched, so a
#      correct answer includes it and a merged delta cannot.
#
# The session is one connection with two identical QUERYs and the rename in between.
# That is the only shape in which a stale cache is visible: the cache lives on the
# connection (design §6.4), so the disk has to change under a session that already has
# a result set, which no two-connection test can do. Before `serve --refresh` existed,
# serve never called esidx_update(), so no answer could go stale -- and cache_matches()
# compared no epoch at all, while esidx.h:485 and design §6.4 both said the cache was
# invalidated by one. Both probes run at once: the refresh is on a wall clock, so run
# sequentially the second server's first QUERY would land after its own first tick and
# the "before" assertion would be measuring something else.

REFRESH_SECS=2
RT_A="$TMP/refa"
RT_B="$TMP/refb"
mkdir -p "$RT_A" "$RT_B"
printf 'x%.0s' $(seq 1 100) >"$RT_A/before.txt"
printf 'x%.0s' $(seq 1 100) >"$RT_B/keep.txt"
printf 'x%.0s' $(seq 1 100) >"$RT_B/before.txt"
RDB_A="$TMP/refa.idx"
RDB_B="$TMP/refb.idx"
"$BIN" build "$RT_A" -o "$RDB_A" 2>/dev/null || { echo "cannot build $RDB_A"; exit 1; }
"$BIN" build "$RT_B" -o "$RDB_B" 2>/dev/null || { echo "cannot build $RDB_B"; exit 1; }

# A flag that cannot mean anything is refused rather than ignored: --refresh=soon parsed
# as 0 would be a server that silently never reconciles, which is indistinguishable from
# a server with nothing to do.
"$BIN" serve "$RDB_A" --refresh=soon >/dev/null 2>"$TMP/bad-refresh.err"
if [ $? -ne 0 ] && grep -q 'needs a number of seconds' "$TMP/bad-refresh.err"; then
    ok "  --refresh=soon is refused rather than read as 0"
else
    bad "  --refresh=soon is refused rather than read as 0" "$(cat "$TMP/bad-refresh.err")"
fi
"$BIN" serve "$RDB_A" --save=5 >/dev/null 2>"$TMP/bad-save.err"
if [ $? -ne 0 ] && grep -q 'needs --refresh' "$TMP/bad-save.err"; then
    ok "  --save without --refresh is refused"
else
    bad "  --save without --refresh is refused" "$(cat "$TMP/bad-save.err")"
fi

SRV_DB="$RDB_A" start_server refresha "--refresh=$REFRESH_SECS"
SRV_DB="$RDB_B" start_server refreshb "--refresh=$REFRESH_SECS" --no-index=size
SRV_DB=

# Both startup repair passes have now run and found nothing -- the file is created after
# them, so the first QUERY below is provably answered from an index that does not have
# it. Asserting that is what keeps the second half of the test from passing vacuously
# if a tick ever lands early.
mv "$RT_A/before.txt" "$RT_A/after.txt"
mv "$RT_B/before.txt" "$RT_B/after.txt"

OUT_A="$TMP/out-a"
OUT_B="$TMP/out-b"

# etp_bg <name> <port> <outfile>: the same session as `etp`, in the background. Its own
# script file, because two probes sharing one would interleave their directives.
etp_bg() {
    local name="$1" port="$2" out="$3"
    local script="$TMP/script-$port"
    cat >"$script"
    timeout 30 "$PROBE" "$port" "$script" >"$out" 2>"$TMP/probe-$port.err"
    echo $? >"$TMP/rc-$port"
}

etp_bg A "$SRV_PORTA" "$OUT_A" <<EOF &
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 50
send EVERYTHING SEARCH parent:"$RT_A" !folder: size:100
sendraw EVERYTHING QUERY
query
sleep $((REFRESH_SECS * 1000 + 1000))
sendraw EVERYTHING QUERY
query
EOF
PIDA=$!

etp_bg B "$SRV_PORTB" "$OUT_B" <<EOF &
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 50
send EVERYTHING SEARCH parent:"$RT_B" !folder: size:100
sendraw EVERYTHING QUERY
query
sleep $((REFRESH_SECS * 1000 + 1000))
sendraw EVERYTHING QUERY
query
EOF
PIDB=$!

wait "$PIDA" "$PIDB"

for p in "$SRV_PORTA" "$SRV_PORTB"; do
    if [ "$(cat "$TMP/rc-$p")" = "0" ]; then
        ok "  a client spanning a refresh on :$p saw no unparseable reply"
    else
        bad "  a client spanning a refresh on :$p saw no unparseable reply" \
            "$(grep -hm3 'PROTOCOL-ERROR' "$OUT_A" "$OUT_B" 2>/dev/null)$(cat "$TMP/probe-$p.err")"
    fi
done

# brow <probe-output>: the row names of the FIRST query block; pnames/lastblock give the
# last one. The "before" half of a session has to be read separately from the "after"
# half, and a buffer that resets at every BLOCK-BEGIN keeps only the last block -- which is
# what lastblock is for, and what made this an empty string the first time.
brow()       { awk '/^BLOCK-BEGIN/ { n++ } n == 1 && /^ROW/ { print }' "$1" \
                 | sed -nE 's/^ROW [0-9]+ [A-Z]+ ([^ ]*).*$/\1/p' | tr '\n' ' '; }

# --- A: the new name, and the new row's size, are both visible on the same connection.
# The first QUERY is asserted stale on purpose. If a tick ever beat it, the section would
# be testing nothing -- so it fails loudly instead of passing vacuously.
expect "  A, before the tick: only the old name" "$(brow "$OUT_A")" "before.txt "
expect "  A, after the tick: the new name, found by its size" "$(pnames "$OUT_A")" "after.txt "
expect "  A, after the tick: still exactly one row" "$(pcount "$OUT_A")" "1"

# --- B: the skipped array stayed unbuilt, so the column fallback answered
expect "  B, before the tick: both 100-byte files, the old name still indexed" \
    "$(brow "$OUT_B")" "before.txt keep.txt "
expect "  B, after the tick: both 100-byte files, the skipped index never became built" \
    "$(pnames "$OUT_B")" "after.txt keep.txt "

# --- the mechanism, not just the answer: the repeat must not have been a cache hit
if grep -q 'cache hit' "$SRVA_ERR"; then
    bad "  the index epoch invalidated the result cache" \
        "$(grep -m2 'cache hit' "$SRVA_ERR")"
else
    ok "  the index epoch invalidated the result cache (design §6.4, esidx.h:485)"
fi
expect "  the startup repair pass ran once, before the listener" \
    "$(grep -c 'startup repair pass' "$SRVA_ERR")" "1"
if grep -q 'sorted index size SKIPPED' "$SRVB_ERR"; then
    ok "  B really did run without the size index (so the guard was under test)"
else
    bad "  B really did run without the size index" "no SKIPPED line in the log"
fi

say "11c. a directory that gained children: the overlay answers, and the drain folds it in"

# `parent:` reads a compressed sparse row, and a shared array has no room in the middle
# (design §5.1) -- so a child added since the last rebuild went into an append-only
# overlay, and di_children() returns both runs. Neither half is observable from the index
# suite, for the reason 11b names: `esidx update` exits and the next process rebuilds the
# array from the columns, so an overlay bug would be invisible there. It needs a process
# that adds and answers in the same breath, which is a server.
#
# The fixture is 48 files rather than the three elsewhere because the drain threshold is
# a *fraction* of the entry count (a twelfth, esidx.h): on a tiny index every addition
# crosses it, so the overlay is emptied before any query could see it and the mechanism
# under test never runs. 48 entries put the threshold at 4, which leaves room for a
# handful of additions to accumulate -- and the "no drain happened" assertion below is
# what proves they did.
OTREE="$TMP/ovl"
OMIT=48
mkdir -p "$OTREE"
i=0; while [ "$i" -lt "$OMIT" ]; do : >"$OTREE/o$i.dat"; i=$((i + 1)); done
ODB="$TMP/ovl.idx"
"$BIN" build "$OTREE" -o "$ODB" 2>/dev/null || { echo "cannot build $ODB"; exit 1; }

SRV_DB="$ODB" start_server overlay "--refresh=$REFRESH_SECS"
OUT_O="$TMP/out-o"

# kids <search>: the row count of one query, run on a fresh connection
kids() {
    cat >"$TMP/script-o" <<EOF
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 1
send EVERYTHING SEARCH $1
sendraw EVERYTHING QUERY
query
EOF
    timeout 30 "$PROBE" "$SRV_PORTO" "$TMP/script-o" >"$OUT_O" 2>&1 || {
        echo "probe failed on '$1'" >&2; return 1; }
    pcount "$OUT_O"
}

# What the filesystem says, so the expected number is not this script's arithmetic.
ofiles() { find "$OTREE" -maxdepth 1 -type f | wc -l; }

expect "  the index starts with the fixture's files" "$(kids "parent:\"$OTREE\" !folder:")" "$(ofiles)"

# --- three additions, under the threshold: the overlay answers, and nothing was rebuilt
for n in 1 2 3; do : >"$OTREE/new$n.dat"; done
sleep $((REFRESH_SECS * 2 + 1))
expect "  three new files are visible to parent:" "$(kids "parent:\"$OTREE\" !folder:")" "$(ofiles)"
# nchild is the count column `child-count:` reads and di_children_n() sums the two runs
# over; they are maintained by the same two functions, so this is the invariant between
# them, and it is the one thing that fails if a caller reads only one of the runs.
expect "  child-count: agrees with parent: on the same directory" \
    "$(kids "child-count:$(ofiles)")" "1"
if grep -q 'drain: children array rebuilt' "$SRVO_ERR"; then
    bad "  the three additions stayed in the overlay (no rebuild ran)" \
        "$(grep -m1 'drain: children array rebuilt' "$SRVO_ERR")"
else
    ok "  the three additions stayed in the overlay, so parent: read both runs"
fi

# --- and removing one that only exists in the overlay. This is the case with a worse
# failure than a wrong count: esidx_remove() walks a subtree through di_remove_child(),
# and a child it cannot find stays a *live* row under a dead parent, which is the one
# state path_of() walks into. No drain has run yet, so the overlay is the only place the
# id can be.
rm -f "$OTREE"/new1.dat "$OTREE"/new2.dat "$OTREE"/new3.dat
sleep $((REFRESH_SECS * 2))
expect "  removing a child that only exists in the overlay" \
    "$(kids "parent:\"$OTREE\" !folder:")" "$(ofiles)"
if grep -q 'drain: children array rebuilt' "$SRVO_ERR"; then
    bad "  the removals were answered from the overlay, not from a rebuild" \
        "$(grep -m1 'drain: children array rebuilt' "$SRVO_ERR")"
else
    ok "  the removals were answered from the overlay, so di_remove_child read it too"
fi

# --- and the name rank, which is the same bargain for the same reason. A rank is a sorted
# position, so a name the index has never seen has nowhere to go: recomputing the order
# costs O(n log n) -- measured 1413 ms over 5.48 M rows -- which is why a pass that added
# one file used to cost more than the whole walk. Those rows instead get a key computed on
# the fly: one binary search over the rank table per *distinct* new name, and nothing at
# all per comparison (query.c, rank_pending).
#
# The three names interleave with the existing ones instead of landing at one end, because
# "sorts before everything" and "sorts in the right place" are different requirements and
# only the second is the contract (design §1.2: a client pages by OFFSET, so the order *is*
# the answer). Two of them land in the same gap, which is the case that needs the count of
# new names sharing a gap rather than a single "before the next rank" slot.
: >"$OTREE/o0a.dat"; : >"$OTREE/o0b.dat"; : >"$OTREE/o1a.dat"
sleep $((REFRESH_SECS * 2 + 1))

# The order, against the oracle that *is* strcasecmp. order-ref is the comparison because
# neither `sort -f` nor `tr A-Z a-z | sort` is (design §10: GNU sort folds for equality
# but orders by the original bytes, and compares signed under LC_ALL=C).
names_sorted() {
    cat >"$TMP/script-o" <<EOF
send USER anonymous
send EVERYTHING SORT name_ascending
send EVERYTHING COUNT 300
send EVERYTHING SEARCH parent:"$OTREE" !folder:
sendraw EVERYTHING QUERY
query
EOF
    timeout 30 "$PROBE" "$SRV_PORTO" "$TMP/script-o" >"$OUT_O" 2>&1 || {
        echo "probe failed" >&2; return 1; }
    pnames "$OUT_O" | tr ' ' '\n' | sed '/^$/d' >"$TMP/os.got"
}
expect_name_order() {   # expect_name_order <label>
    names_sorted
    find "$OTREE" -maxdepth 1 -type f | awk -F/ '{print $NF}' >"$TMP/os.want"
    "$ORDER_REF" "$TMP/os.want" >"$TMP/os.want.sorted"
    if cmp -s "$TMP/os.got" "$TMP/os.want.sorted"; then
        ok "  $1"
    else
        bad "  $1" "$(diff "$TMP/os.want.sorted" "$TMP/os.got" | head -6 | tr '\n' ' ')"
    fi
}
if [ -x "$ORDER_REF" ]; then
    expect_name_order "a name sort with three unranked names interleaved matches strcasecmp order"
else
    bad "  a name sort with three unranked names interleaved matches strcasecmp order" \
        "$ORDER_REF not built"
fi
if grep -q 'drain: name rank rebuilt' "$SRVO_ERR"; then
    bad "  those three names were ordered without a rank rebuild" \
        "$(grep -m1 'drain: name rank rebuilt' "$SRVO_ERR")"
else
    ok "  those three names were ordered without a rank rebuild (computed on the fly)"
fi
expect "  and the count is still the filesystem's" \
    "$(kids "parent:\"$OTREE\" !folder:")" "$(ofiles)"

# --- now far past any threshold: the drain folds the overlay into the array, and the
# answers must not move. The count is compared against find(1), not against what the
# server said a moment ago, so a wrong answer cannot agree with itself.
BULK=$((OMIT * 4))
i=0; while [ "$i" -lt "$BULK" ]; do : >"$OTREE/bulk$i.dat"; i=$((i + 1)); done
echo "   $BULK files created under $OTREE"
sleep $((REFRESH_SECS * 3))
expect "  after a bulk create the count is the filesystem's" \
    "$(kids "parent:\"$OTREE\" !folder:")" "$(ofiles)"
if grep -q 'drain: children array rebuilt' "$SRVO_ERR"; then
    ok "  the drain ran once the overlay passed its threshold ($(grep -c 'drain: children array rebuilt' "$SRVO_ERR") children, $(grep -c 'drain: name rank rebuilt' "$SRVO_ERR") rank)"
else
    bad "  the drain ran once the overlay passed its threshold" "no drain line in the log"
fi
# The same comparison again, now that the ranks have been recomputed instead of computed
# per row. Same tree, same oracle, so a self-consistent-but-wrong key cannot pass this one:
# the on-the-fly keys and the rebuilt ranks have to agree with strcasecmp about the same
# 243 names.
if [ -x "$ORDER_REF" ]; then
    expect_name_order "and the order still matches strcasecmp after the drain recomputed the ranks"
fi
expect "  child-count: still agrees after the drain" \
    "$(kids "child-count:$(ofiles)")" "1"

# --- and removals, which never needed the overlay: they swap-remove in place
rm -f "$OTREE"/bulk*.dat
sleep $((REFRESH_SECS * 2))
expect "  after removing the bulk the count is the filesystem's" \
    "$(kids "parent:\"$OTREE\" !folder:")" "$(ofiles)"
expect "  child-count: agrees after the removals" \
    "$(kids "child-count:$(ofiles)")" "1"

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
