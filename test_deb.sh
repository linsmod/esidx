#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
#
# Install the built .deb on this machine, watch it serve, and purge it again.
#
#   sudo ./test_deb.sh                      # the newest dist/esidx_*.deb
#   sudo ./test_deb.sh dist/esidx_X_amd64.deb
#   sudo TREE=/work ./test_deb.sh           # index another tree (default /etc)
#
# This is the *install* half of what `make deb-verify` cannot reach: deb-verify unpacks the
# package in /tmp and runs what came out, which says the contents are executable and the unit
# parses -- not that postinst, the unit, systemd and the kernel agree. Those only meet when
# dpkg installs it, so this runs the whole cycle on the machine it is invoked on and takes
# the package away again (dpkg -P). It prints each step and asserts the state that step is
# supposed to leave behind.
#
# Requirements: root (dpkg, systemd and fanotify all need it), Linux, a free ESIDX_PORT
# (2121), and a machine whose /etc you are willing to index for a minute. It is not part of
# `make check`: the gate must not install packages on the builder.
#
# Measured against r7000 (Ubuntu 22.04, the baseline host) when it was written -- 0 failures,
# and the numbers in that commit message came from here.

set -u
export DEBIAN_FRONTEND=noninteractive

cd "$(dirname "$0")"

DEB=${1:-$(ls -1t dist/esidx_*.deb 2>/dev/null | head -1)}
TREE=${TREE:-/etc}
PORT=${ESIDX_PORT:-2121}
DB=/var/lib/esidx/root.idx
PROBE=${PROBE:-./etp-probe}
S=${TMPDIR:-/tmp}/esidx-deb-test.script
# postinst's output is kept (its warnings are the only place some refusals are visible), so
# it needs a directory that exists on every run -- found the hard way: the script used the
# ambient $TMP and ran on a host that happened to export one, then died at dpkg -i on a host
# that did not.
TMP=${TMPDIR:-/tmp}/esidx-deb-test
mkdir -p "$TMP"
FAILED=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; FAILED=$((FAILED + 1)); }
note() { printf '        %s\n' "$*"; }
stage() { printf '\n########## %s ##########\n' "$*"; }

probe() {   # probe <search> -> the probe's output
    printf 'send USER anonymous\nsend EVERYTHING COUNT 5\nsend EVERYTHING SEARCH %s\n' "$1" >"$S"
    printf 'sendraw EVERYTHING QUERY\nquery\nclose\n' >>"$S"
    "$PROBE" "$PORT" "$S" 2>&1
}

[ "$(id -u)" -eq 0 ] || { echo "test_deb.sh: run it as root (dpkg, systemd, fanotify)" >&2; exit 2; }
[ -n "$DEB" ] && [ -f "$DEB" ] || { echo "test_deb.sh: no package -- run 'make deb' first" >&2; exit 2; }

# The probe has to be *runnable*, and a copied one usually is not: `scp` does not carry the
# mode, so a 0664 probe fails with EACCES at the first query and looks exactly like a server
# that does not answer. So it is settled here by running it -- no-argument, which prints its
# usage -- and if it cannot run the two steps that need it are skipped with the reason printed
# rather than reported as failures ("did not run" and "passed" must not look the same).
HAVE_PROBE=1
if [ ! -x "$PROBE" ]; then
    make etp-probe >/dev/null 2>&1 || true
fi
if ! { [ -x "$PROBE" ] && "$PROBE" 2>&1 | grep -q '^usage'; }; then
    HAVE_PROBE=0
    echo "test_deb.sh: $PROBE is not runnable; the wire and event steps will be SKIPPED" >&2
    echo "             (build it here with 'make etp-probe', or point PROBE= at one)" >&2
fi

stage "0. what the package says it is"
dpkg-deb -I "$DEB" | sed -n '/^Description/,$p' | sed 's/^/   /'

stage "1. before: nothing of ours is here"
getent passwd esidx >/dev/null 2>&1 && bad "an esidx user already exists -- not a clean machine" \
                                     || ok "no esidx user"
[ -e /usr/lib/systemd/system/esidx.service ] && bad "a unit is already installed" \
                                            || ok "no unit"
ss -ltn 2>/dev/null | grep -q ":$PORT " && bad "port $PORT is busy" || ok "port $PORT is free"

stage "2. dpkg -i (postinst output verbatim)"
# Kept, not just printed: postinst's warnings are the only place some of its refusals are
# visible at all, and an assertion about a warning needs the text. The install itself must
# not be asserted on here -- this machine is not the deployment (there is no /work), so what
# is checked below is that it *complained*, not that it succeeded quietly.
dpkg -i "$DEB" >"$TMP/install.log" 2>&1
sed 's/^/   /' "$TMP/install.log"

stage "3. what postinst left behind"
getent passwd esidx >/dev/null 2>&1 \
    && ok "user: $(id -un esidx) uid=$(id -u esidx) gid=$(id -g esidx)" || bad "no esidx user"
stat -c '        /var/lib/esidx: %U:%G %a' /var/lib/esidx 2>/dev/null || bad "/var/lib/esidx missing"
[ -f /etc/default/esidx ] && ok "configuration /etc/default/esidx" || bad "no /etc/default/esidx"
[ -f /etc/esidx/roots ] && ok "locations /etc/esidx/roots" || bad "no /etc/esidx/roots"
[ -f /usr/lib/systemd/system/esidx.service ] && ok "unit" || bad "no unit"
# Both are conffiles, so an upgrade leaves an edited copy alone. The unit reads the locations
# file at startup as well as every 5 s (--reload-config), and compares it with what the
# snapshot covers -- but a dpkg upgrade that overwrote it would still silently discard a
# deployment's configuration.
for c in /etc/default/esidx /etc/esidx/roots; do
    if grep -qx "$c" /var/lib/dpkg/info/esidx.conffiles 2>/dev/null; then
        ok "$c is a conffile"
    else
        bad "$c is not in dpkg's conffile list"
    fi
done
note "is-enabled: $(systemctl is-enabled esidx 2>&1)"
# The packaged roots file names /work, which does not exist on a test machine: that is the
# deployment's business, not the package's, and postinst is expected to warn about exactly
# this rather than fail. So the assertion is that it *warns*, not that the path exists.
grep -q 'esidx: warning: .*esidx/roots names' "$TMP/install.log" 2>/dev/null \
    && ok "postinst warns about a location that does not exist here" \
    || bad "postinst did not warn about /etc/esidx/roots naming a missing path"

stage "4. the first snapshot -- built by the service, not by this script"
# No build command is run here, and that is the assertion: writing the locations file and
# (re)starting the unit is the whole of a deployment now (design D11). The server compares
# /etc/esidx/roots with what its snapshot covers at startup, and on a fresh install there is
# no snapshot at all -- which is a disagreement, so it builds one, in a child process, as the
# user it drops to. That identity is the point: the index is a view of what *it* may read,
# and a build run by this script as root would quietly produce a different one.
printf '%s\n' "$TREE" > /etc/esidx/roots
note "locations: $(tr '\n' ' ' < /etc/esidx/roots)"
systemctl restart esidx
n=0
while [ "$n" -lt 60 ]; do
    [ -f "$DB" ] && break
    n=$((n + 1)); sleep 1
done
if [ -f "$DB" ]; then
    ok "the service built it in ${n}s: $(stat -c '%U:%G %s bytes' "$DB")"
else
    bad "no snapshot after ${n}s -- the service did not build one"
    journalctl -u esidx --no-pager -n 20 | sed 's/^/   /'
fi

stage "4b. a restart that has nothing to do"
# The other half of the same rule: after the swap the snapshot carries the locations it was
# built from, so a start that finds the two in agreement must not scan anything again. The
# file's size and mtime are the observable -- a rebuild would rewrite it.
before=$(stat -c '%Y %s' "$DB" 2>/dev/null)
systemctl restart esidx
sleep 3
after=$(stat -c '%Y %s' "$DB" 2>/dev/null)
[ "$before" = "$after" ] && ok "no rebuild: file and snapshot already agree" \
                         || bad "the snapshot changed on a restart that had nothing to do"

stage "5. the running service"
[ "$(systemctl is-active esidx)" = active ] && ok "active" || bad "state: $(systemctl is-active esidx)"
journalctl -u esidx --no-pager -n 10 | sed 's/^/   /'
pid=$(pgrep -x esidx | head -1)
if [ -n "$pid" ]; then
    ps -o user=,pid=,args= -p "$pid" | sed 's/^/   /'
    awk '/^Uid|^Gid|^CapEff/{print "   " $0}' /proc/"$pid"/status
    # 0000000000000004 is CAP_DAC_READ_SEARCH and nothing else: the whole point of the drop.
    grep -q '^CapEff:\s*0000000000000004$' /proc/"$pid"/status \
        && ok "holds exactly CAP_DAC_READ_SEARCH" || bad "unexpected capability set"
else
    bad "no esidx process"
fi

# What the bind and the address list come to, read from the running unit rather than the file:
# these are the two settings that decide who can reach the index, and the package ships both.
bind=$(sed -n 's/^ESIDX_BIND=//p' /etc/default/esidx)
listen=$(ss -ltnH 2>/dev/null | awk -v p=":$PORT" '$4 ~ p {print $4; exit}')
case "$listen" in
    "")                 bad "nothing is listening on $PORT" ;;
    *"$bind"*)          ok "listening on $listen (ESIDX_BIND=$bind)" ;;
    *)                  bad "listening on $listen but ESIDX_BIND=$bind" ;;
esac
note "systemd: $(systemctl show esidx -p IPAddressDeny -p IPAddressAllow --value | tr '\n' ' ')"



stage "6. the wire"
if [ "$HAVE_PROBE" = 1 ]; then
    probe 'ext:conf' | head -8 | sed 's/^/   /'
else
    note "SKIPPED: no runnable etp-probe"
fi

stage "7. events, end to end"
if [ "$HAVE_PROBE" = 1 ]; then
    touch "$TREE/esidx-deb-test-file"
    sleep 2
    r1=$(probe 'esidx-deb-test-file' | grep -c '^ROW ' || true)
    rm -f "$TREE/esidx-deb-test-file"
    sleep 2
    r2=$(probe 'esidx-deb-test-file' | grep -c '^ROW ' || true)
    if [ "$r1" = 1 ] && [ "$r2" = 0 ]; then
        ok "created -> 1 row, removed -> 0 rows: the event reached the served index"
    else
        bad "created -> $r1 rows (want 1), removed -> $r2 rows (want 0)"
    fi
    journalctl -u esidx --no-pager -n 4 | sed 's/^/   /'
else
    note "SKIPPED: no runnable etp-probe"
fi

stage "7b. the locations file: add one, no restart"
# The packaged unit passes --reload-config=/etc/esidx/roots, so editing that file must reach
# the *running* server: a stat every 5 s, a rebuild in a child, a swap without a restart. The
# pid is what separates a reload from a restart -- a restart would also show the new
# location, after a much bigger gap -- so it is captured before and asserted after.
if [ "$HAVE_PROBE" = 1 ]; then
    TREE2=${TREE2:-/usr/share/doc}
    if [ ! -d "$TREE2" ]; then
        note "SKIPPED: TREE2=$TREE2 does not exist"
    elif [ "${TREE2#"$TREE"}" != "$TREE2" ] || [ "${TREE#"$TREE2"}" != "$TREE" ]; then
        note "SKIPPED: TREE2=$TREE2 overlaps TREE=$TREE (a root inside another is refused)"
    else
        # A name that lives in TREE2 and not under TREE, picked rather than hardcoded so the
        # step does not depend on one particular doc file existing everywhere.
        name=$(find "$TREE2" -maxdepth 2 -type f -printf '%f\n' 2>/dev/null | grep -m1 . || true)
        if [ -z "$name" ]; then
            note "SKIPPED: nothing to search for under $TREE2"
        else
            pid0=$(systemctl show -p MainPID --value esidx)
            r0=$(probe "$name" | grep -c '^ROW ' || true)
            if [ "$r0" != 0 ]; then
                note "SKIPPED: '$name' is already indexed via $TREE; pick another TREE2"
            else
                printf '%s\n%s\n' "$TREE" "$TREE2" > /etc/esidx/roots
                r1=""; n=0
                # 5 s to notice + a full scan of both trees; the loop is generous because
                # this machine is not the baseline host.
                for i in $(seq 1 40); do
                    sleep 3
                    n=$i
                    r1=$(probe "$name" | grep -c '^ROW ' || true)
                    [ "$r1" -gt 0 ] && break
                done
                pid1=$(systemctl show -p MainPID --value esidx)
                if [ "$r1" -gt 0 ]; then
                    ok "a location added to /etc/esidx/roots is served after ~$((n*3)) s"
                else
                    bad "a location added to /etc/esidx/roots was never served"
                fi
                # D11: the marks are placed before the privilege drop and cannot be moved
                # after it, so a swap onto locations they do not cover costs one automatic
                # restart -- the process stops itself, systemd brings it back (Restart=always)
                # and the new process marks the new set before it drops. That is the design
                # now, so "MainPID unchanged" is not the assertion; the journal line saying
                # *why* is, together with what the restart buys (below).
                if [ -n "$pid0" ] && [ "$pid0" = "$pid1" ]; then
                    ok "in one process (MainPID $pid1 unchanged) -- the marks already covered it"
                elif journalctl -u esidx --no-pager -n 400 2>/dev/null \
                        | grep -q 'stopping so systemd restarts this service'; then
                    ok "restarted on purpose ($pid0 -> $pid1), and the journal says why"
                else
                    bad "MainPID changed ($pid0 -> $pid1) with no line saying why"
                fi
                # What the restart buys, which is the real assertion: the new location is
                # event-driven rather than sweep-only. A file created under TREE2 has to be
                # served in seconds; the sweep is hourly (ESIDX_SWEEP=3600), so anything that
                # only the sweep could have found would not arrive inside this loop.
                if [ -w "$TREE2" ]; then
                    tag="esidx-deb-tree2-$$"
                    touch "$TREE2/$tag"
                    e1=0
                    for i in $(seq 1 10); do
                        sleep 2
                        e1=$(probe "$tag" | grep -c '^ROW ' || true)
                        [ "$e1" -gt 0 ] && break
                    done
                    rm -f "$TREE2/$tag"
                    [ "$e1" -gt 0 ] \
                        && ok "and it is event-driven: a file under the new location appears without a sweep" \
                        || bad "a file created under $TREE2 never appeared -- the new location is sweep-only"
                else
                    note "SKIPPED: $TREE2 is not writable here"
                fi
            fi
        fi
    fi
else
    note "SKIPPED: no runnable etp-probe"
fi

stage "8. purge, and what it takes away"
systemctl stop esidx
sleep 1
dpkg -P esidx 2>&1 | sed 's/^/   /'
getent passwd esidx >/dev/null 2>&1 && bad "user still there" || ok "user gone"
[ -d /var/lib/esidx ] && bad "state directory still there" || ok "state directory gone"
[ -e /etc/default/esidx ] && bad "configuration still there" || ok "configuration gone"
[ -e /etc/esidx/roots ] && bad "locations file still there" || ok "locations file gone"
[ -e /usr/lib/systemd/system/esidx.service ] && bad "unit still there" || ok "unit gone"
[ -e /usr/bin/esidx ] && bad "esidx still there" || ok "esidx gone"
[ -e /usr/bin/sfa-server ] && bad "sfa-server still there" || ok "sfa-server gone"
note "is-enabled: $(systemctl is-enabled esidx 2>&1)"

rm -f "$S"
stage "result: $FAILED failure(s)"
exit "$FAILED"
