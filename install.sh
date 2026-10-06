#!/usr/bin/env bash
#
# Install esidx and its event source, and optionally start them.
#
# The two halves install together on purpose. esidx is useless with `--watch` and no
# proxy, and the proxy is useless with no client, and the fiddly part of a deployment was
# never the copying -- it was remembering that one of them needs CAP_SYS_ADMIN and the
# other must not have it, that the socket's group is what makes that split work
# (sfa issues/closed/socket-permissions.md), and that a bare `git clone` of esidx has an
# empty sfa/ because the submodule was never initialised. All three are handled here.
#
#   ./install.sh                          # build + install into /usr/local, print how to run
#   ./install.sh --prefix ~/.local        # same, no root needed
#   ./install.sh --start --root /work     # ...and start the pair, indexing /work
#   ./install.sh --destdir /tmp/stage     # stage a package instead of installing
#   ./install.sh --stop                   # stop what --start started
#
set -eu

PREFIX=/usr/local
DESTDIR=
GROUP=
ROOTDIR=
DBPATH=
PORT=2121
SOCKET=
LOGDIR=
DO_START=0
DO_STOP=0
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
        --group)   GROUP=$2; shift 2 ;;
        --group=*)  GROUP=${1#*=}; shift ;;
        --root)    ROOTDIR=$2; shift 2 ;;
        --root=*)  ROOTDIR=${1#*=}; shift ;;
        --db)      DBPATH=$2; shift 2 ;;
        --db=*)    DBPATH=${1#*=}; shift ;;
        --with-debug) WITH_DEBUG=1; shift ;;
        --sudo-stdin) SUDO_STDIN=1; shift ;;
        --port)    PORT=$2; shift 2 ;;
        --port=*)  PORT=${1#*=}; shift ;;
        --socket)  SOCKET=$2; shift 2 ;;
        --logdir)  LOGDIR=$2; shift 2 ;;
        --logdir=*) LOGDIR=${1#*=}; shift ;;
        --jobs)    JOBS=$2; shift 2 ;;
        --start)   DO_START=1; shift ;;
        --stop)    DO_STOP=1; shift ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         die "unknown option $1 (try --help)" ;;
    esac
done

cd "$(dirname "$0")"
[ -f sfa/sfa.h ] || die "sfa/ is empty -- this tree has no event source (in a git checkout: git submodule update --init --recursive)"

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

# The proxy needs CAP_SYS_ADMIN, so `--start` needs root exactly once, at the beginning,
# and the ticket then covers every later call. Three ways to be here, and the difference
# matters because the failure otherwise arrives four steps later as sudo's own message:
#   a terminal       -- `sudo -v` prompts once, which is the normal case
#   --sudo-stdin     -- the password arrives on stdin, for `ssh host 'install.sh --start'`
#   neither          -- refused here, with the two ways out named
if [ "$DO_START" = 1 ] && [ "$(id -u)" -ne 0 ]; then
    if [ "$SUDO_STDIN" = 1 ]; then
        sudo -S -v >/dev/null 2>&1 || true      # consumes one line of stdin
    elif [ -t 0 ]; then
        say "-- the event proxy needs root; one password prompt follows"
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
# sfa's install is what puts sfa-server on PATH; without it there is no --watch, so it is
# not optional here even though it was optional in the build.
$SUDO_INSTALL make -C sfa install PREFIX="$PREFIX" DESTDIR="$DESTDIR"

# ---------------------------------------------------------------- run

if [ "$DO_STOP" = 1 ]; then
    say "== stopping"
    $SUDO_RUN pkill -x sfa-server 2>/dev/null || true
    pkill -x esidx 2>/dev/null || true
    say "   stopped (proxy and server)"
fi

if [ "$DO_START" != 1 ]; then
    [ "$DO_STOP" = 1 ] && exit 0
    say ""
    say "installed. Snapshot path: $DBPATH"
    say "To index and serve ${ROOTDIR:-/} with live events:"
    say "  sudo $PREFIX/bin/sfa-server ${ROOTDIR:-/} --group $(id -un) $SOCKET   # privileged half"
    say "  $PREFIX/bin/esidx build ${ROOTDIR:-/} -o $DBPATH                      # once"
    say "  $PREFIX/bin/esidx serve $DBPATH -p $PORT --watch=$SOCKET --sweep=3600"
    say "or let this script do all three:  ./install.sh --start --root ${ROOTDIR:-/}"
    exit 0
fi

# ---------------------------------------------------------------- start the pair

[ -n "$ROOTDIR" ] || die "--start needs --root: the tree to index (e.g. --root /work)"
command -v "$PREFIX/bin/sfa-server" >/dev/null 2>&1 \
    || [ -x "$PREFIX/bin/sfa-server" ] || die "no sfa-server under $PREFIX/bin"
command -v "$PREFIX/bin/esidx" >/dev/null 2>&1 \
    || [ -x "$PREFIX/bin/esidx" ] || die "no esidx under $PREFIX/bin"

# The group is the whole privilege split: the proxy runs as root, the index server does
# not, and the socket's group is what lets the second one connect to the first. Default
# to the invoking user's own primary group, which always exists and always matches them.
[ -n "$GROUP" ] || GROUP=$(id -gn)
getent group "$GROUP" >/dev/null 2>&1 || die "no such group: $GROUP"

mkdir -p "$LOGDIR" 2>/dev/null || $SUDO_RUN mkdir -p "$LOGDIR"

say "== stop anything left over, then start clean"
$SUDO_RUN pkill -x sfa-server 2>/dev/null || true
pkill -x esidx 2>/dev/null || true
sleep 0.5
rm -f "$SOCKET"

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
say "stop:    sudo pkill -x sfa-server; pkill -x esidx"
say "query:   nc 127.0.0.1 $PORT   (or any ETP client; see README)"