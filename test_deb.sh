#!/usr/bin/env bash
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
dpkg -i "$DEB" 2>&1 | sed 's/^/   /'

stage "3. what postinst left behind"
getent passwd esidx >/dev/null 2>&1 \
    && ok "user: $(id -un esidx) uid=$(id -u esidx) gid=$(id -g esidx)" || bad "no esidx user"
stat -c '        /var/lib/esidx: %U:%G %a' /var/lib/esidx 2>/dev/null || bad "/var/lib/esidx missing"
[ -f /etc/default/esidx ] && ok "configuration /etc/default/esidx" || bad "no /etc/default/esidx"
[ -f /usr/lib/systemd/system/esidx.service ] && ok "unit" || bad "no unit"
note "is-enabled: $(systemctl is-enabled esidx 2>&1)"
root_val=$(sed -n 's/^ESIDX_ROOT=//p' /etc/default/esidx)
note "ESIDX_ROOT=$root_val   (exists here: $([ -d "$root_val" ] && echo yes || echo no))"

stage "4. the first snapshot, as the service user"
sudo -u esidx /usr/bin/esidx build "$TREE" -o "$DB" 2>&1 | tail -2 | sed 's/^/   /'
[ -f "$DB" ] && ok "snapshot: $(stat -c '%U:%G %s bytes' "$DB")" || bad "no snapshot"

stage "5. systemctl start"
systemctl start esidx
sleep 3
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

stage "8. purge, and what it takes away"
systemctl stop esidx
sleep 1
dpkg -P esidx 2>&1 | sed 's/^/   /'
getent passwd esidx >/dev/null 2>&1 && bad "user still there" || ok "user gone"
[ -d /var/lib/esidx ] && bad "state directory still there" || ok "state directory gone"
[ -e /etc/default/esidx ] && bad "configuration still there" || ok "configuration gone"
[ -e /usr/lib/systemd/system/esidx.service ] && bad "unit still there" || ok "unit gone"
[ -e /usr/bin/esidx ] && bad "esidx still there" || ok "esidx gone"
[ -e /usr/bin/sfa-server ] && bad "sfa-server still there" || ok "sfa-server gone"
note "is-enabled: $(systemctl is-enabled esidx 2>&1)"

rm -f "$S"
stage "result: $FAILED failure(s)"
exit "$FAILED"
