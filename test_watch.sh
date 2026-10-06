#!/usr/bin/env bash
#
# The watcher suite: kernel events -> dirty directories -> visible rows.
#
#   ./test_watch.sh                run against ./esidx (or ESIDX_BIN)
#   ESIDX_BUILD=dbg ./test_watch.sh  run against the sanitiser build
#
# Root and a working sfa-server are both required, and the script says so and skips
# rather than failing: `make check` must stay green on a machine where the watcher cannot
# run at all, which is every machine without CAP_SYS_ADMIN. That is also why this is a
# separate suite from test_etp.sh (AGENTS.md 1.3) -- the protocol suite pins the wire and
# must not need a privilege the protocol does not.
#
# What is asserted, and why this shape:
#
#   1. The visible rows, over the protocol, by name. This is the only assertion that
#      matters to a user, and it is the same oracle the other two suites use.
#   2. The *dirty set*, read out of the server's own log line. The event -> directory
#      mapping is the part that can be subtly wrong while the rows still come out right
#      (marking too much is correct but slow; marking the child instead of the parent is
#      correct only because the parent also gets marked). Pinning the backend itself is
#      the difference between "it worked" and "it worked for the reason we think".
#   3. The negative cases: an event outside the root must not be counted, and a proxy
#      that goes away must be reported rather than silently ignored -- the latter is the
#      difference between an index that is current and one that has quietly stopped being
#      current, which is why esidx_watch_drain() returns -2 and not 0.

set -u
cd "$(dirname "$0")"

case "${ESIDX_BUILD:-opt}" in
    dbg) BIN=${ESIDX_BIN:-./esidx-dbg}; PROBE=./etp-probe-dbg ;;
    opt) BIN=${ESIDX_BIN:-./esidx};    PROBE=./etp-probe ;;
    *)   printf 'ESIDX_BUILD must be opt or dbg, not "%s"\n' "${ESIDX_BUILD}" >&2; exit 2 ;;
esac

if [ ! -x "$BIN" ]; then
    printf 'test_watch.sh: %s not built -- run `make`\n' "$BIN" >&2
    exit 1
fi

# Refuse to measure a binary that is older than the code. A build that fails part-way
# leaves the previous binary in place, and every assertion below would then pass against
# the *last* build while reporting it as this one -- which is the same shape as the run
# that looked sanitised and was not (AGENTS.md 3.1), and it cost the same hour to find.
# The suite cannot know what is inside the binary; this is the cheapest thing it can know.
for f in *.c *.h Makefile sfa/*.c sfa/*.h; do
    [ -f "$f" ] || continue
    if [ "$f" -nt "$BIN" ]; then
        printf 'test_watch.sh: %s is newer than %s -- build did not run or did not finish\n' \
            "$f" "$BIN" >&2
        exit 1
    fi
done
if [ ! -x "$PROBE" ]; then
    printf 'test_watch.sh: %s not built -- run `make`\n' "$PROBE" >&2
    exit 1
fi
if [ ! -x sfa/sfa-server ]; then
    printf 'test_watch.sh: sfa/sfa-server not built -- run `make -C sfa`\n' >&2
    exit 1
fi

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/esidx-watch.XXXXXX")
SOCK="$TMP/sfa.sock"
# TEST_KEEP=1 leaves the tree, the log and the socket behind, which is the only way to read
# what the server actually said about a failed assertion.
cleanup() {
    [ -n "${SFA_PID:-}" ]   && kill "$SFA_PID"  2>/dev/null
    [ -n "${SERVE_PID:-}" ] && kill "$SERVE_PID" 2>/dev/null
    [ -n "${EMB_PID:-}" ]   && kill "$EMB_PID"  2>/dev/null
    [ -n "${EMB2_PID:-}" ]  && kill "$EMB2_PID" 2>/dev/null
    wait 2>/dev/null
    if [ "${TEST_KEEP:-0}" = 1 ]; then
        printf 'test_watch.sh: kept %s\n' "$TMP"
    else
        rm -rf "$TMP"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- capability

if ! ./sfa/sfa-server --probe "$TMP" >"$TMP/probe.txt" 2>&1; then
    printf 'test_watch.sh: this machine cannot run the watcher; skipping.\n'
    sed 's/^/  /' "$TMP/probe.txt"
    exit 0
fi
printf 'test_watch.sh: watcher available\n'

# ---------------------------------------------------------------- fixture

ROOT="$TMP/tree"
mkdir -p "$ROOT/sub/deep" "$ROOT/empty"
printf 'one\n'   > "$ROOT/alpha.txt"
printf 'two\n'   > "$ROOT/sub/beta.txt"
printf 'three\n' > "$ROOT/sub/deep/gamma.txt"
# A decoy outside the root, on the same filesystem, so the prefix filter is exercised by a
# real event rather than by a unit test on a string.
printf 'decoy\n' > "$TMP/outside.txt"

DB="$TMP/tree.idx"
"$BIN" build "$ROOT" -o "$DB" >"$TMP/build.log" 2>&1 || {
    printf 'test_watch.sh: build failed\n'; sed 's/^/  /' "$TMP/build.log"; exit 1; }

./sfa/sfa-server "$TMP" "$SOCK" >"$TMP/sfa.log" 2>&1 &
SFA_PID=$!
sleep 0.5

"$BIN" -v 3 serve "$DB" -p 0 --bind 127.0.0.1 --watch="$SOCK" \
    >"$TMP/serve.out" 2>"$TMP/serve.log" &
SERVE_PID=$!

# The port is only in the log; wait for the banner rather than sleeping a fixed time, so a
# slow load does not turn into a flaky test.
PORT=""
for _ in $(seq 1 50); do
    PORT=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$TMP/serve.log" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    sleep 0.1
done
if [ -z "$PORT" ]; then
    printf 'test_watch.sh: the server never came up\n'
    sed 's/^/  /' "$TMP/serve.log"
    exit 1
fi
printf 'test_watch.sh: esidx on 127.0.0.1:%s, sfa on %s\n' "$PORT" "$SOCK"

# One connection per query, driven through the same probe and the same script format the
# protocol suite uses (AGENTS.md 3.3). Two details of that format are not optional and both
# make an assertion vacuous if missed: `sendraw EVERYTHING QUERY` only *sends*, and the
# `query` directive on the next line is what reads the result block; and a semicolon-
# separated command string is not a script at all.
query() {
    printf 'send USER anonymous\nsend EVERYTHING CASE 0\nsend EVERYTHING REGEX 0\nsend EVERYTHING WHOLE_WORD 0\nsend EVERYTHING SIZE_COLUMN 1\nsend EVERYTHING DATE_MODIFIED_COLUMN 1\nsend EVERYTHING PATH_COLUMN 1\nsend EVERYTHING SORT name_ascending\nsend EVERYTHING OFFSET 0\nsend EVERYTHING COUNT 20\nsend EVERYTHING SEARCH %s\nsendraw EVERYTHING QUERY\nquery\nclose\n' \
        "$1" >"$TMP/q.script"
    timeout 20 "$PROBE" "$PORT" "$TMP/q.script" 2>/dev/null
}

# Wait until a name shows up, or give up. A fixed sleep would be a guess; the loop is the
# thing that makes the assertion about the mechanism rather than about the clock -- and the
# number it finally reports is the latency measurement.
await_name() {
    local want=$1 tries=${2:-50} i
    for i in $(seq 1 "$tries"); do
        if query "name:$want" | grep -qE "^ROW [0-9]+ [A-Z]+ $want( |$)"; then
            echo "$i"
            return 0
        fi
        sleep 0.1
    done
    echo "-1"
    return 1
}

# Wait until a name stops being answered. The loop is the whole assertion: "absent once" is
# not "deleted", and with the coalescing window a delete is applied ~50 ms after the event,
# so the first poll legitimately still sees the row. This version returned on the first
# *sighting*, which passed only while the delete happened to be applied before the first
# poll -- an assertion that was right by accident (AGENTS.md 3.4).
await_absent() {
    local want=$1 i
    for i in $(seq 1 50); do
        query "name:$want" | grep -qE "^ROW [0-9]+ [A-Z]+ $want( |$)" || return 0
        sleep 0.1
    done
    return 1
}

# ---------------------------------------------------------------- assertions

# 1. a file created one level down, before the server's next batch, is visible
printf 'four\n' > "$ROOT/delta.txt"
N=$(await_name delta.txt)
if [ "$N" != "-1" ]; then
    ok "a create is visible within the batch (after ${N} poll interval(s))"
else
    bad "a create is visible" "name:delta.txt never appeared"
fi

# 2. the same, two levels down: the event names the file, and the directory to list is
#    the one holding it -- which is not the root.
printf 'five\n' > "$ROOT/sub/deep/epsilon.txt"
N=$(await_name epsilon.txt)
if [ "$N" != "-1" ]; then
    ok "a create two levels down is visible (after ${N} poll interval(s))"
else
    bad "a create two levels down is visible" "name:epsilon.txt never appeared"
fi

# 3. a new directory, arriving with nothing in it
mkdir "$ROOT/sub/fresh"
N=$(await_name fresh)
if [ "$N" != "-1" ]; then
    ok "a new directory is visible (after ${N} poll interval(s))"
else
    bad "a new directory is visible" "name:fresh never appeared"
fi

# 4. ... and the same directory once it has content, which is the ref B5 case: nothing
#    marked the new directory itself, so the parent has to descend into it
printf 'six\n' > "$ROOT/sub/fresh/zeta.txt"
N=$(await_name zeta.txt)
if [ "$N" != "-1" ]; then
    ok "a file inside a newly created directory is visible (after ${N} poll interval(s))"
else
    bad "a file inside a new directory is visible" "name:zeta.txt never appeared"
fi

# 5. a delete
rm "$ROOT/alpha.txt"
if await_absent alpha.txt; then
    ok "a delete is applied"
else
    bad "a delete is applied" "name:alpha.txt still answered"
fi

# 6. a rename: one event carrying both paths, so both the old row and the new one change
mv "$ROOT/sub/beta.txt" "$ROOT/sub/beta-renamed.txt"
N=$(await_name beta-renamed.txt)
if [ "$N" != "-1" ] && await_absent beta.txt; then
    ok "a rename is one mark of each side (after ${N} poll interval(s))"
else
    bad "a rename is applied to both sides" "old row still answered, or the new one never appeared"
fi

# 7. a delete of a whole subtree: the events for what was inside it arrive after the
#    directory is gone, and marking a directory that no longer exists must not be an error
mkdir -p "$ROOT/doomed/inner"
printf 'x\n' > "$ROOT/doomed/inner/one.txt"
printf 'y\n' > "$ROOT/doomed/inner/two.txt"
await_name one.txt >/dev/null
rm -rf "$ROOT/doomed"
if await_absent doomed; then
    ok "a subtree delete removes the directory"
else
    bad "a subtree delete removes the directory" "name:doomed still answered"
fi
sleep 0.5

# 8. a bulk change in one directory, from parallel writers. This is the assertion that
#    decides whether the layer is usable: the dirty set is de-duplicated per *batch*, and
#    without the coalescing window a fast writer produces one event per batch, so the cost
#    is one reconcile per event. Measured before the window existed, same fixture: 1000
#    files -> 975 batches -> 975 directory listings, ~0.5 s of a loop that answers nobody.
#    After: 4 batches, 4 listings, ~6 ms. The oracle is the ratio, not the absolute count,
#    because the absolute count depends on how fast the writer is.
mkdir -p "$ROOT/bulk"
LINES_BEFORE=$(wc -l <"$TMP/serve.log")
seq 1 1000 | xargs -P8 -I{} sh -c "echo x > '$ROOT/bulk/f{}.txt'"
N=$(await_name f1000.txt)
if [ "$N" != "-1" ]; then
    ok "1000 files created at once are visible (after ${N} poll interval(s))"
else
    bad "a bulk create is visible" "name:f1000.txt never appeared"
fi
sleep 1.0
# Only the lines after the burst started: summing the whole log would count the earlier
# assertions' directories and make a passing ratio look like a failure.
SUM=$(tail -n "+$((LINES_BEFORE + 1))" "$TMP/serve.log" | awk '
/watch: .* applied .* mark/ {
    for (i = 1; i <= NF; i++) {
        if ($i == "applied") mk += $(i + 1);
        if ($i == "over")   dr += $(i + 1);
        if ($i == "events") { ev = $(i - 1); gsub(/[^0-9]/, "", ev); }
    }
    nb++;
}
END { printf "%d %d %d %d\n", nb + 0, mk + 0, dr + 0, ev + 0 }')
set -- $SUM
NB=${1:-0}; NMK=${2:-0}; NDR=${3:-0}; NEV=${4:-0}
printf 'test_watch.sh: burst: %s event(s) -> %s mark(s) -> %s applied batch(es) over %s director(s)\n' \
    "$NEV" "$NMK" "$NB" "$NDR"
# The oracle is the ratio, and the threshold is 50 marks per listing: the pre-coalescing
# behaviour was 1:1, so anything near that fails loudly, while the shape of the fixture (a
# root reconcile descends into its subdirectories and is counted as more than one) cannot
# make a working run fail.
if [ "$NMK" -ge 900 ] && [ $((NDR * 50)) -le "$NMK" ] && [ "$NB" -le 8 ]; then
    ok "a burst of $NMK marks costs $NDR listing(s) ($((NMK / (NDR > 0 ? NDR : 1))) marks each), not $NMK"
else
    bad "a burst is one listing per directory, not per event" \
        "$NMK mark(s) over $NDR director(s) in $NB batch(es)"
fi

# 9. the dirty set itself: an event outside the root must be seen and discarded, not
#    marked, and a create inside the root must be marked exactly once per batch.
: > "$TMP/decoy.txt"
printf 'more\n' >> "$TMP/outside.txt"
printf 'seven\n' > "$ROOT/eta.txt"
sleep 1.0
if grep -q 'applied [0-9]* mark' "$TMP/serve.log"; then
    ok "the watcher logs the batch it applied"
else
    bad "the watcher logs the batch it applied" "no 'watch: ... applied N mark(s) over' line"
fi
if grep -qE '[1-9][0-9]* outside the root' "$TMP/serve.log"; then
    ok "an event outside the root is counted as outside, not marked"
else
    bad "an event outside the root is discarded" "no non-zero 'outside the root' count"
fi

# 10. an attribute-only change must NOT claim to be covered. touch moves size? No: mtime.
#    The index is not expected to follow it, and the banner says so.
touch "$ROOT/eta.txt"
sleep 0.8
if grep -q 'size/mtime still follow' "$TMP/serve.out" "$TMP/serve.log" 2>/dev/null; then
    ok "the startup banner states what the watcher does not cover"
else
    bad "the banner states the attribute caveat" "no such line in the banner"
fi

# 10b. the banner must carry the proxy's own report of how it negotiated. One assertion for
#      the SDK call rather than for the text: the mark mode is the answer to "why am I sent
#      events for a filesystem I do not index" (FAN_MARK_FILESYSTEM covers a whole
#      filesystem and is the fallback where the mount form is rejected), and it is in the
#      handshake precisely so a client can print it.
if grep -qE 'watching .* via .* \[(MARK_MOUNT|FILESYSTEM)' "$TMP/serve.out" "$TMP/serve.log" 2>/dev/null; then
    ok "the banner reports the proxy's mark mode"
else
    bad "the banner reports the proxy's mark mode" \
        "no '[MARK_MOUNT|...]' in the banner -- is the handshake still sfa_connect()?"
fi

# 11. a rename the proxy cannot deliver, and the only reason we ever learn about it.
#     sfa puts a rename's two paths in one 4096-byte buffer and drops the event if they do
#     not both fit, so a rename at ~2100 characters produces no MOVED at all -- not a wrong
#     one, none. Nothing below the top of a deleted tree behaves any better, and there the
#     outermost rmdir always survives to mark the parent, which is why assertion 7 passes
#     without this one. A rename has no such survivor, so the loss signal is the only thing
#     that can tell this index "dst.dat exists now" -- and the signal only reaches a client
#     that subscribed to it, because the proxy filters it on the mask like any other event.
#     Take the bit out of SFA_WATCH_MASK and this fails.
#
#     What the signal buys is the repair, and this is the assertion that says so: a rename
#     the proxy could not deliver has no event at all, so if the loss did not turn into a
#     sweep then dst.dat could not appear by any route -- with --watch and no --refresh
#     there is no periodic pass to find it later either. Before the sweep existed this
#     failed, because the answer to a loss signal was a mark of the root and the root's pass
#     only descends into directories whose stamp moved.
DEEP="$ROOT/deep"
mkdir -p "$DEEP"
SEG=$(printf 'd%.0s' $(seq 1 60))
while [ "${#DEEP}" -lt 2100 ]; do
    DEEP="$DEEP/$SEG"
    mkdir -p "$DEEP"
done
printf 'deep\n' > "$DEEP/src.dat"
if await_name src.dat >/dev/null; then
    ok "a file at ${#DEEP} characters of path depth is visible before the rename"
else
    bad "the deep fixture is indexed" "src.dat at ${#DEEP} characters never appeared"
fi
mv "$DEEP/src.dat" "$DEEP/dst.dat"
N=$(await_name dst.dat)
if [ "$N" != "-1" ] && await_absent src.dat; then
    ok "a rename the proxy cannot resolve still converges (after ${N} poll interval(s))"
else
    bad "a rename the proxy cannot resolve still converges" \
        "dst.dat never appeared, or src.dat still answers -- the loss did not become a sweep"
fi
if grep -q 'could not attribute to a path' "$TMP/serve.log"; then
    ok "the loss is reported, not absorbed"
else
    bad "the loss is reported" \
        "no 'could not attribute to a path' line -- is SFA_EV_UNRESOLVED still subscribed?"
fi
if grep -qE 'sweep: [0-9]+ director\(ies\) compared, [1-9][0-9]* moved' "$TMP/serve.log"; then
    ok "the loss is answered with a sweep, and the sweep found the directory"
else
    bad "the loss is answered with a sweep" "no sweep line reporting a moved directory"
fi

# 12. the proxy going away must be reported. This is the assertion that keeps a dead
#     watcher from looking like an idle one.
kill "$SFA_PID" 2>/dev/null
wait "$SFA_PID" 2>/dev/null
SFA_PID=""
sleep 0.8
if grep -q 'the sfa proxy at fd .* is gone' "$TMP/serve.log"; then
    ok "a proxy that goes away is reported, not ignored"
else
    bad "a proxy that goes away is reported" "no 'is gone' line in the log"
fi

# 13. and the server must still answer afterwards: dropping the watcher is not a crash,
#     and the index it holds is intact. It must NOT learn about a new file -- with the
#     proxy gone there is nothing telling it, which is exactly what assertion 10 says is
#     now visible in the log. Asserting the opposite here would be asserting a lie.
if query 'name:gamma.txt' | grep -qE '^ROW [0-9]+ [A-Z]+ gamma\.txt( |$)'; then
    ok "the server still answers, from the index it had, after the watcher is dropped"
else
    bad "the server still answers after the watcher is dropped" "an existing row stopped answering"
fi
printf 'eight\n' > "$ROOT/theta.txt"
sleep 0.8
if query 'name:theta.txt' | grep -qE '^ROW [0-9]+ [A-Z]+ theta\.txt( |$)'; then
    bad "a change with no watcher is NOT picked up" "theta.txt appeared with the proxy dead"
else
    ok "a change made with the proxy dead is not claimed to be indexed"
fi

# ---------------------------------------------------------------- embedded form
#
# One process for the whole event path: this server holds the fanotify group itself and
# gives the privilege back before it opens the listener. Two assertions are here that the
# socket form cannot express, and each is the reason the other mode is not a substitute:
#
#   1. **The drop happened, and kept exactly one capability** -- read from the outside, out
#      of /proc, rather than from the log line the code prints about itself. The failure
#      guarded is a drop that silently did less; watch.c's own step 5 asks the same question
#      from the inside. CAP_DAC_READ_SEARCH is the one that must survive: without it
#      open_by_handle_at fails and every batch becomes a full sweep (measured on r7000, the
#      numbers are in watch.c).
#   2. **The loss signal reaches the in-process callback.** The callback is a second delivery
#      path for the same events, and the loss signals are where the two can disagree -- an
#      UNRESOLVED that reached a socket client but not the callback would make this mode the
#      one that never sweeps, which is silent by construction.
#
# Its own tree and its own snapshot, because everything above is about a server whose proxy
# was killed and whose privilege was never given up.
EMB="$TMP/emb"; EROOT="$EMB/tree"
mkdir -p "$EROOT/sub"
# The dropped process has to be able to read the tree and the snapshot, and `mktemp -d`
# makes $TMP 0700 root -- the same trap that made a fixture unwritable during the r7000
# deployment work.
chmod 0755 "$TMP" "$EMB"
printf 'one\n' > "$EROOT/a.txt"
printf 'two\n' > "$EROOT/sub/b.txt"
EDB="$EMB/tree.idx"
"$BIN" build "$EROOT" -o "$EDB" >"$EMB/build.log" 2>&1 || {
    printf 'test_watch.sh: embedded fixture build failed\n'; sed 's/^/  /' "$EMB/build.log"; }
chmod 0644 "$EDB"

DROP_USER=${ESIDX_DROP_TO:-nobody}
# The snapshot is chowned to the drop target, which is what the packaged deployment looks
# like (`sudo -u esidx esidx build ...`). That is deliberate here: --drop-to defaults to the
# snapshot's owner, so making the two the same user is what lets the assertions below test
# the *derivation* rather than a value that was typed twice.
chown "$DROP_USER" "$EDB"
"$BIN" -v 3 serve "$EDB" -p 0 --bind 127.0.0.1 --watch-embed \
    >"$EMB/serve.out" 2>"$EMB/serve.log" &
EMB_PID=$!
PORT=""
for _ in $(seq 1 50); do
    PORT=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$EMB/serve.log" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    sleep 0.1
done
if [ -n "$PORT" ]; then
    ok "the embedded server comes up on 127.0.0.1:$PORT with no proxy at all"
else
    bad "the embedded server comes up" "no banner: $(tail -2 "$EMB/serve.log" | tr '\n' ' ')"
fi

if [ -n "$PORT" ] && [ -r "/proc/$EMB_PID/status" ]; then
    if grep -q "becoming $DROP_USER, the owner of" "$EMB/serve.log" 2>/dev/null; then
        ok "the user to become defaults to the snapshot's owner (--drop-to omitted)"
    else
        bad "the drop target defaults to the snapshot's owner" \
            "no 'becoming ... owner of' line -- is --drop-to still required, or is the default silent?"
    fi
    WANT_UID=$(id -u "$DROP_USER" 2>/dev/null)
    UID_NOW=$(sed -n 's/^Uid:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/$EMB_PID/status")
    if [ -z "$WANT_UID" ]; then
        printf 'test_watch.sh: no user %s on this machine; the drop assertions are skipped\n' \
            "$DROP_USER"
    elif [ "$UID_NOW" = "$WANT_UID" ]; then
        ok "the embedded server runs as $DROP_USER (uid $UID_NOW), not root"
    else
        bad "the embedded server gives up its uid" "Uid: $UID_NOW, expected $WANT_UID"
    fi
    # CapEff is printed as 16 hex digits, so the expected value is exact: bit 2
    # (CAP_DAC_READ_SEARCH) and nothing else. Anything wider means the drop did less than
    # the mode promises; anything narrower means event paths can no longer be resolved.
    CAP=$(sed -n 's/^CapEff:[[:space:]]*//p' "/proc/$EMB_PID/status")
    if [ "$CAP" = "0000000000000004" ]; then
        ok "exactly CAP_DAC_READ_SEARCH is kept (CapEff $CAP)"
    else
        bad "the capability set is one capability" \
            "CapEff: $CAP, expected 0000000000000004 (CAP_DAC_READ_SEARCH)"
    fi
    # The user is part of the banner on purpose: the line that reports the drop is at INFO,
    # and a default run shows no INFO -- so a banner without it leaves "which identity is
    # this service now" unanswerable at the level the service actually logs at.
    if grep -q "via the embedded fanotify group (this process), as $DROP_USER" \
            "$EMB/serve.out" "$EMB/serve.log" 2>/dev/null; then
        ok "the banner names the embedded group and the user it became"
    else
        bad "the banner names the group and the user it became" \
            "no '... (this process), as $DROP_USER' line -- is the identity reported only at INFO?"
    fi
fi

if [ -n "$PORT" ]; then
    printf 'three\n' > "$EROOT/gamma.txt"
    N=$(await_name gamma.txt)
    if [ "$N" != "-1" ]; then
        ok "a create is visible in embedded mode (after ${N} poll interval(s))"
    else
        bad "a create is visible in embedded mode" "name:gamma.txt never appeared"
    fi

    rm "$EROOT/a.txt"
    if await_absent a.txt; then
        ok "a delete is applied in embedded mode"
    else
        bad "a delete is applied in embedded mode" "name:a.txt still answered"
    fi

    mv "$EROOT/sub/b.txt" "$EROOT/sub/b-renamed.txt"
    N=$(await_name b-renamed.txt)
    if [ "$N" != "-1" ] && await_absent b.txt; then
        ok "a rename is applied to both sides in embedded mode (after ${N} poll interval(s))"
    else
        bad "a rename is applied in embedded mode" "old row still answered, or the new one never appeared"
    fi

    # The loss path, which is the one assertion that separates "the callback works" from
    # "the callback is a second source of truth": a rename the proxy cannot put in one
    # buffer produces no event at all, so dst.dat can only appear if SFA_EV_UNRESOLVED
    # reached on_event and the watcher answered it with a sweep.
    EDEEP="$EROOT/deep"
    mkdir -p "$EDEEP"
    ESEG=$(printf 'e%.0s' $(seq 1 60))
    while [ "${#EDEEP}" -lt 2100 ]; do
        EDEEP="$EDEEP/$ESEG"
        mkdir -p "$EDEEP"
    done
    printf 'deep\n' > "$EDEEP/src.dat"
    if await_name src.dat >/dev/null; then
        ok "a ${#EDEEP}-character path is indexed in embedded mode"
    else
        bad "the deep fixture is indexed in embedded mode" "src.dat never appeared"
    fi
    mv "$EDEEP/src.dat" "$EDEEP/dst.dat"
    N=$(await_name dst.dat)
    if [ "$N" != "-1" ] && await_absent src.dat; then
        ok "a loss signal reaches the in-process callback (after ${N} poll interval(s))"
    else
        bad "the loss signal reaches the in-process callback" \
            "dst.dat never appeared, or src.dat still answers -- is on_event_mask still SFA_WATCH_MASK?"
    fi
    if grep -q 'could not attribute to a path' "$EMB/serve.log"; then
        ok "the embedded watcher reports the loss"
    else
        bad "the embedded watcher reports the loss" "no 'could not attribute to a path' line"
    fi
    if grep -qE 'sweep: [0-9]+ director\(ies\) compared, [1-9][0-9]* moved' "$EMB/serve.log"; then
        ok "the embedded watcher answers the loss with a sweep"
    else
        bad "the embedded watcher answers the loss with a sweep" "no sweep line reporting a moved directory"
    fi

    # One snapshot is served by one process (main.c's singleton lock), so the first embedded
    # server has to go before the second one starts -- which is the honest sequence anyway: the
    # spelled-out form is the same deployment with its arguments written out, not a second one.
    # Without this the two tests below would fail on the lock instead of on what they check.
    kill "$EMB_PID" 2>/dev/null; wait "$EMB_PID" 2>/dev/null; EMB_PID=""

    # Both defaults can still be spelled out, and that is what keeps them defaults rather
    # than the only way: the unit names the user explicitly, and a caller who wants the tree
    # checked against what they think they deployed names it too.
    "$BIN" -v 3 serve "$EDB" -p 0 --bind 127.0.0.1 \
        --watch-embed="$EROOT" --drop-to="$DROP_USER" \
        >"$EMB/serve2.out" 2>"$EMB/serve2.log" &
    EMB2_PID=$!
    PORT2=""
    for _ in $(seq 1 50); do
        PORT2=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$EMB/serve2.log" 2>/dev/null | head -1)
        [ -n "$PORT2" ] && break
        sleep 0.1
    done
    if [ -n "$PORT2" ]; then
        ok "the spelled-out form (--watch-embed=ROOT --drop-to=USER) still starts"
    else
        bad "the spelled-out form still starts" "$(tail -2 "$EMB/serve2.log" | tr '\n' ' ')"
    fi
    kill "$EMB2_PID" 2>/dev/null; wait "$EMB2_PID" 2>/dev/null; EMB2_PID=""

    # ...and naming a tree that is not the snapshot's root is refused, which is the only
    # reason spelling it out exists: it is a statement about the deployment that can be wrong.
    if "$BIN" -v 3 serve "$EDB" -p 0 --bind 127.0.0.1 --watch-embed="$TMP" \
            >/dev/null 2>"$EMB/serve3.log"; then
        bad "a --watch-embed naming another tree is refused" "it started anyway"
    elif grep -q "but the snapshot's root is" "$EMB/serve3.log"; then
        ok "a --watch-embed naming another tree is refused, naming both paths"
    else
        bad "a tree that is not the snapshot's root is refused" \
            "$(tail -2 "$EMB/serve3.log" | tr '\n' ' ')"
    fi
fi

printf 'test_watch.sh: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1