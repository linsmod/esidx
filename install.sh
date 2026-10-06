#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 linsmod <linsmod@qq.com>
#
# Install esidx and its event source, and optionally start it.
#
# `--start` starts the *one-process* form: one server opens the fanotify group as root, gives
# the capability back before it opens the listener, and the tree it marks comes from the
# snapshot itself. That is the shape the .deb ships and the shape README's deployment section
# describes, so it is the default here too. `--split` asks for the two-process form instead
# (root proxy + unprivileged server + socket between them), which stays the right shape where
# the index server must not even *start* with a capability.
#
# Both halves are still installed, always. The submodule is compiled into the esidx binary
# (the embedded watcher is sfa's own client code), so a checkout without sfa/ cannot build at
# all; and the headers + SDK + sfa-server it installs are what a downstream consumer links
# against and what `--split` needs at run time. What was fiddly by hand was never the copying
# -- it was the socket's group and mode in the split form (sfa
# issues/closed/socket-permissions.md) and the empty sfa/ in a bare clone.
#
#   ./install.sh                          # build + install into /usr/local, print how to run
#   ./install.sh --prefix ~/.local        # same, no root needed for the install
#   ./install.sh --start --root /work     # ...and start it, indexing /work
#   ./install.sh --start --root /work --split       # the two-process form instead
#   ./install.sh --start --root /work --drop-to=svc # ...become another user (default: yours)
#   ./install.sh --destdir /tmp/stage     # stage a package instead of installing
#   ./install.sh --stop                   # stop what --start started
#
set -eu

PREFIX=/usr/local
DESTDIR=
GROUP=
GROUP_GIVEN=0
ROOTDIR=
DBPATH=
PORT=2121
SOCKET=
SOCKET_GIVEN=0
LOGDIR=
DROP_TO=
DO_START=0
DO_STOP=0
DO_SPLIT=0
WITH_DEBUG=0
SUDO_STDIN=0
JOBS=$(nproc 2>/dev/null || echo 4)

die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)  PREFIX=$2; shift 2 ;;
        --prefix=*) PREFIX=${1#*=}; shift ;;
        --destdir)  DESTDIR=$2; shift 2 ;;
        --destdir=*) DESTDIR=${1#*=}; shift ;;
        --group)   GROUP=$2; GROUP_GIVEN=1; shift 2 ;;
        --group=*)  GROUP=${1#*=}; GROUP_GIVEN=1; shift ;;
        --root)    ROOTDIR=$2; shift 2 ;;
        --root=*)  ROOTDIR=${1#*=}; shift ;;
        --db)      DBPATH=$2; shift 2 ;;
        --db=*)    DBPATH=${1#*=}; shift ;;
        --drop-to) DROP_TO=$2; shift 2 ;;
        --drop-to=*) DROP_TO=${1#*=}; shift ;;
        --with-debug) WITH_DEBUG=1; shift ;;
        --sudo-stdin) SUDO_STDIN=1; shift ;;
        --port)    PORT=$2; shift 2 ;;
        --port=*)  PORT=${1#*=}; shift ;;
        --socket)  SOCKET=$2; SOCKET_GIVEN=1; shift 2 ;;
        --logdir)  LOGDIR=$2; shift 2 ;;
        --logdir=*) LOGDIR=${1#*=}; shift ;;
        --jobs)    JOBS=$2; shift 2 ;;
        --start)   DO_START=1; shift ;;
        --stop)    DO_STOP=1; shift ;;
        --split)   DO_SPLIT=1; shift ;;
        -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         die "unknown option $1 (try --help)" ;;
    esac
done

cd "$(dirname "$0")"
[ -f sfa/sfa.h ] || die "sfa/ is empty -- this tree has no event source (in a git checkout: git submodule update --init --recursive)"

# ---------------------------------------------------------------- check what was asked for
#
# Everything here is about intent and needs no files, so it happens before anything is built
# and before any privilege is asked for: a --group that belongs to another form, or a --start
# as root with no user to become, is a mistake worth refusing while it is still cheap to fix
# -- and refusing it after a sudo prompt is not cheap.

if [ "$DO_START" = 1 ]; then
    [ -n "$ROOTDIR" ] || die "--start needs --root: the tree to index (e.g. --root /work)"

    # --group and --socket describe the split form and nothing else: there is no socket in the
    # one-process form, so there is no group to put on one. Refusing beats ignoring -- an
    # ignored --group reads as "the split is arranged for" at the one moment it is not.
    if [ "$DO_SPLIT" != 1 ] && { [ "$GROUP_GIVEN" = 1 ] || [ "$SOCKET_GIVEN" = 1 ]; }; then
        die "--group/--socket belong to the split form: add --split, or drop them (there is no socket without it)"
    fi

    if [ "$DO_SPLIT" = 1 ]; then
        [ -n "$DROP_TO" ] && die "--drop-to belongs to the one-process form; --split runs the index server as you"
    else
        # Who the server becomes. It defaults to the invoking user for the same reason the
        # binary defaults to the snapshot's owner: that is who builds the snapshot below, so
        # the identity does not change with the shape of the deployment -- only who holds the
        # capability does. As root there is no such answer (the target must not be root), so it
        # is asked for rather than guessed, which is also what the binary does with a
        # root-owned snapshot.
        [ -n "$DROP_TO" ] || DROP_TO=$(id -un)
        [ "$DROP_TO" != root ] || die "--start as root needs --drop-to=USER: giving root back is the point of the one-process form"
        getent passwd "$DROP_TO" >/dev/null 2>&1 || die "no such user: $DROP_TO"
    fi
fi

# Privilege is two separate questions and conflating them is a bug I wrote and then deployed:
# "may I write the install prefix" and "will something here need root" are unrelated. The
# prefix can be a user directory while the proxy still needs CAP_SYS_ADMIN, and a system
# prefix can be installed by a root invocation that then needs no further privilege.
SUDO_INSTALL=""
SUDO_RUN=""
[ "$(id -u)" -ne 0 ] && SUDO_RUN=sudo
if [ "$(id -u)" -ne 0 ] && [ -z "$DESTDIR" ] && [ ! -w "$PREFIX" ]; then
    command -v sudo >/dev/null 2>&1 \
        || die "$PREFIX is not writable and there is no sudo -- use --prefix ~/.local"
    SUDO_INSTALL=sudo
fi

[ -n "$SOCKET" ] || SOCKET=/tmp/esidx-sfa.sock
[ -n "$LOGDIR" ] || LOGDIR=/tmp/esidx-logs

# Opening the fanotify group needs CAP_SYS_ADMIN in both forms -- the proxy in --split, this
# server itself without it -- so `--start` needs root exactly once, at the beginning, and the
# ticket then covers every later call. Three ways to be here, and the difference matters
# because the failure otherwise arrives four steps later as sudo's own message:
#   a terminal       -- `sudo -v` prompts once, which is the normal case
#   --sudo-stdin     -- the password arrives on stdin, for `ssh host 'install.sh --start'`
#   neither          -- refused here, with the two ways out named
if [ "$DO_START" = 1 ] && [ "$(id -u)" -ne 0 ]; then
    if [ "$SUDO_STDIN" = 1 ]; then
        sudo -S -v >/dev/null 2>&1 || true      # consumes one line of stdin
    elif [ -t 0 ]; then
        say "-- opening the fanotify group needs root; one password prompt follows"
        sudo -v || die "sudo failed"
    else
        die "--start needs root and there is no terminal to ask on.
       Either run this from a terminal, or run 'sudo -v' first (the ticket lasts about
       15 minutes), or pipe the password in with --sudo-stdin."
    fi
fi

# The snapshot is data, not a system file, so it defaults somewhere the invoking user can
# write. /var/lib is the right place for a package install, and the wrong default for a
# per-user prefix install -- which is what made the first deployment attempt die on
# `mkdir /var/lib/esidx` with no explanation.
if [ -z "$DBPATH" ]; then
    if [ "$(id -u)" -eq 0 ] && [ -z "$DESTDIR" ] && [ "$PREFIX" = /usr/local ]; then
        DBPATH=/var/lib/esidx/root.idx
    else
        DBPATH="${XDG_DATA_HOME:-$HOME/.local/share}/esidx/root.idx"
    fi
fi

# ---------------------------------------------------------------- build and install


# `make opt`, not a bare `make`: the default goal builds both flavours and the sanitiser one
# is the slowest thing on the target by a wide margin, and an install needs one binary. The
# watcher suite and the gate want the other flavour and run it themselves.
say "== build (make opt -j$JOBS)"
if [ "$WITH_DEBUG" = 1 ]; then
    make -j"$JOBS"
else
    make opt -j"$JOBS"
fi

say "== install esidx -> $DESTDIR$PREFIX/bin"
$SUDO_INSTALL make install PREFIX="$PREFIX" DESTDIR="$DESTDIR"

say "== install the event source (headers + SDK + the proxy) -> $DESTDIR$PREFIX"
# Both halves in full, even though the default start uses only what is already inside the
# esidx binary: the headers and the static SDK are what a consumer of this install links
# against, and sfa-server on PATH is what --split runs. The build needed sfa/ either way.
$SUDO_INSTALL make -C sfa install PREFIX="$PREFIX" DESTDIR="$DESTDIR"

# ---------------------------------------------------------------- run

if [ "$DO_STOP" = 1 ]; then
    say "== stopping"
    pkill -x esidx 2>/dev/null || true
    $SUDO_RUN pkill -x sfa-server 2>/dev/null || true
    # Reported as done only once it is done. SIGTERM is handled by leaving the poll loop
    # between two turns, and a turn can be a whole timeout -- so "stopped" printed straight
    # after the signal is a claim about the future, and it was wrong often enough to be
    # noticed the first time `--stop` was checked (verification script: still running, right
    # after the line saying otherwise).
    for _ in $(seq 30); do
        if ! pgrep -x esidx >/dev/null && ! pgrep -x sfa-server >/dev/null; then break; fi
        sleep 0.1
    done
    if pgrep -x esidx >/dev/null || pgrep -x sfa-server >/dev/null; then
        LEFT=$( { pgrep -x esidx; pgrep -x sfa-server; } 2>/dev/null | tr '\n' ' ')
        say "   still running after 3 s -- pids: $LEFT"
        say "   (send SIGKILL by hand if one is wedged; ./install.sh --stop again does not help)"
        exit 1
    fi
    say "   stopped (the server, and the proxy if a --split run left one)"
fi

if [ "$DO_START" != 1 ]; then
    [ "$DO_STOP" = 1 ] && exit 0
    say ""
    say "installed. Snapshot path: $DBPATH"
    say "To index and serve ${ROOTDIR:-/} with live events, in one process:"
    say "  $PREFIX/bin/esidx build ${ROOTDIR:-/} -o $DBPATH        # once, needs no privilege"
    say "  sudo $PREFIX/bin/esidx serve $DBPATH --watch-embed      # opens the group as root, drops to you"
    say "or with the proxy split, where the index server must not even start privileged:"
    say "  sudo $PREFIX/bin/sfa-server ${ROOTDIR:-/} --group $(id -gn) $SOCKET"
    say "  $PREFIX/bin/esidx serve $DBPATH --watch=$SOCKET"
    say "or let this script do it:  ./install.sh --start --root ${ROOTDIR:-/}   (add --split for the second form)"
    exit 0
fi

# ---------------------------------------------------------------- start

command -v "$PREFIX/bin/esidx" >/dev/null 2>&1 \
    || [ -x "$PREFIX/bin/esidx" ] || die "no esidx under $PREFIX/bin"
if [ "$DO_SPLIT" = 1 ]; then
    command -v "$PREFIX/bin/sfa-server" >/dev/null 2>&1 \
        || [ -x "$PREFIX/bin/sfa-server" ] || die "no sfa-server under $PREFIX/bin (--split is the form that needs it)"
fi

mkdir -p "$LOGDIR" 2>/dev/null || $SUDO_RUN mkdir -p "$LOGDIR"

say "== stop anything left over, then start clean"
pkill -x esidx 2>/dev/null || true
$SUDO_RUN pkill -x sfa-server 2>/dev/null || true
sleep 0.5
if [ "$DO_SPLIT" = 1 ]; then rm -f "$SOCKET"; fi

if [ ! -f "$DBPATH" ]; then
    say "== building the first snapshot of $ROOTDIR into $DBPATH (this is the slow step)"
    # No sudo by default: the snapshot path defaults under the invoking user's own data
    # directory precisely so that the slow step needs no privilege. sudo appears here only
    # when someone pointed --db somewhere else that we cannot write.
    DBBUILD=""
    if ! mkdir -p "$(dirname "$DBPATH")" 2>/dev/null; then
        [ -n "$SUDO_RUN" ] || die "cannot create $(dirname "$DBPATH") -- pass --db PATH somewhere writable, or run this with a tty so sudo can ask"
        $SUDO_RUN mkdir -p "$(dirname "$DBPATH")"
        DBBUILD="$SUDO_RUN"
    fi
    # shellcheck disable=SC2086
    $DBBUILD "$PREFIX/bin/esidx" build "$ROOTDIR" -o "$DBPATH"
fi

if [ "$DO_SPLIT" = 1 ]; then
    # The group is the whole privilege split: the proxy runs as root, the index server does
    # not, and the socket's group is what lets the second one connect to the first. Default
    # to the invoking user's own primary group, which always exists and always matches them.
    [ -n "$GROUP" ] || GROUP=$(id -gn)
    getent group "$GROUP" >/dev/null 2>&1 || die "no such group: $GROUP"

    say "== 1/2 the privileged half: sfa-server as root, marked on $ROOTDIR"
    $SUDO_RUN setsid nohup "$PREFIX/bin/sfa-server" "$ROOTDIR" --group "$GROUP" "$SOCKET" \
        >"$LOGDIR/sfa.log" 2>&1 </dev/null &
    for _ in $(seq 50); do [ -S "$SOCKET" ] && break; sleep 0.2; done
    [ -S "$SOCKET" ] || { say "   FAILED to start; see $LOGDIR/sfa.log"; exit 1; }
    ls -l "$SOCKET" | sed 's/^/   /'
    say "   negotiated: $(grep -o 'mark 模式.*' "$LOGDIR/sfa.log" | head -1)"

    say "== 2/2 the unprivileged half: esidx as $(id -un), watching it"
    setsid nohup "$PREFIX/bin/esidx" -v 3 serve "$DBPATH" -p "$PORT" --bind 127.0.0.1 \
        --watch="$SOCKET" --sweep=3600 >"$LOGDIR/esidx.log" 2>&1 </dev/null &
    for _ in $(seq 100); do grep -q 'esidx serving' "$LOGDIR/esidx.log" 2>/dev/null && break; sleep 0.2; done
    grep -E 'esidx serving|esidx: watching' "$LOGDIR/esidx.log" | sed 's/^/   /'
    grep -q 'esidx serving' "$LOGDIR/esidx.log" || { say "   FAILED to start; see $LOGDIR/esidx.log"; exit 1; }

    say ""
    say "running.  index server: 127.0.0.1:$PORT (no privilege)   proxy: root, socket $SOCKET"
    say "logs:    $LOGDIR/{sfa,esidx}.log"
    say "stop:    ./install.sh --stop"
    say "query:   nc 127.0.0.1 $PORT   (or any ETP client; see README)"
    exit 0
fi

# --------------------------------------------- start (default: one process, as the .deb runs it)

say "== starting: esidx on 127.0.0.1:$PORT as $DROP_TO, fanotify group opened as root"
# --watch-embed takes the tree from the snapshot rather than from the command line, so
# $ROOTDIR is used only to build the snapshot above. That is the whole alignment with the
# packaged unit: the tree is stated once, by the file, and there is no second place to be wrong.
$SUDO_RUN setsid nohup "$PREFIX/bin/esidx" -v 3 serve "$DBPATH" -p "$PORT" --bind 127.0.0.1 \
    --watch-embed --drop-to="$DROP_TO" --sweep=3600 >"$LOGDIR/esidx.log" 2>&1 </dev/null &
for _ in $(seq 100); do grep -q 'esidx serving' "$LOGDIR/esidx.log" 2>/dev/null && break; sleep 0.2; done
grep -E 'esidx serving|esidx: watching|watch: dropped to' "$LOGDIR/esidx.log" | sed 's/^/   /'
grep -q 'esidx serving' "$LOGDIR/esidx.log" || { say "   FAILED to start; see $LOGDIR/esidx.log"; exit 1; }

# The snapshot is read before the drop (as root) and written, if at all, after it -- so a
# snapshot owned by someone else is not fatal at startup, but --save would be. Startup is the
# last moment that can name it, and a hint here beats a failed write weeks later.
DBOWNER=$(stat -c '%U' "$DBPATH" 2>/dev/null || echo '?')
if [ "$DBOWNER" != "$DROP_TO" ]; then
    say "   note: $DBPATH is owned by $DBOWNER, not $DROP_TO -- readable, but --save could not write it"
fi

say ""
say "running.  server: 127.0.0.1:$PORT as $DROP_TO (root at exec, capability given back)"
say "logs:    $LOGDIR/esidx.log"
say "stop:    ./install.sh --stop"
say "query:   nc 127.0.0.1 $PORT   (or any ETP client; see README)"