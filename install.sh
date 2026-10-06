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
PORT=2121
SOCKET=
LOGDIR=
DO_START=0
DO_STOP=0
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

# Privilege: installing under a prefix we cannot write, and starting the proxy at all,
# both need root. Ask once and reuse, rather than putting sudo in front of every command
# and letting it prompt four times.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if [ -w "$PREFIX" ] || [ -n "$DESTDIR" ]; then
        :                                   # writable prefix: no sudo needed for the install
    elif command -v sudo >/dev/null 2>&1; then
        SUDO=sudo
        say "-- prefix $PREFIX is not writable; the privileged steps will ask for a password"
    else
        die "$PREFIX is not writable and there is no sudo -- use --prefix ~/.local"
    fi
fi
[ -n "$SOCKET" ] || SOCKET=/tmp/esidx-sfa.sock
[ -n "$LOGDIR" ] || LOGDIR=/tmp/esidx-logs

# ---------------------------------------------------------------- build and install

say "== build (make -j$JOBS)"
make -j"$JOBS"

say "== install esidx -> $DESTDIR$PREFIX/bin"
$SUDO make install PREFIX="$PREFIX" DESTDIR="$DESTDIR"

say "== install the event source (headers + SDK + the proxy) -> $DESTDIR$PREFIX"
# sfa's install is what puts sfa-server on PATH; without it there is no --watch, so it is
# not optional here even though it was optional in the build.
$SUDO make -C sfa install PREFIX="$PREFIX" DESTDIR="$DESTDIR"

# ---------------------------------------------------------------- run

if [ "$DO_STOP" = 1 ]; then
    say "== stopping"
    $SUDO pkill -x sfa-server 2>/dev/null || true
    pkill -x esidx 2>/dev/null || true
    say "   stopped (proxy and server)"
fi

if [ "$DO_START" != 1 ]; then
    [ "$DO_STOP" = 1 ] && exit 0
    say ""
    say "installed. To index $DESTDIR$PREFIX and serve it with live events:"
    say "  sudo $PREFIX/bin/sfa-server ${ROOTDIR:-/} --group $(id -un) $SOCK   # privileged half"
    say "  $PREFIX/bin/esidx build ${ROOTDIR:-/} -o /var/lib/esidx/root.idx  # once"
    say "  $PREFIX/bin/esidx serve /var/lib/esidx/root.idx -p $PORT --watch=$SOCK --sweep=3600"
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

mkdir -p "$LOGDIR" 2>/dev/null || $SUDO mkdir -p "$LOGDIR"

say "== stop anything left over, then start clean"
$SUDO pkill -x sfa-server 2>/dev/null || true
pkill -x esidx 2>/dev/null || true
sleep 0.5
rm -f "$SOCKET"

DB=/var/lib/esidx/root.idx
if [ ! -f "$DB" ]; then
    say "== building the first snapshot of $ROOTDIR (this is the slow step)"
    $SUDO mkdir -p "$(dirname "$DB")"
    $SUDO "$PREFIX/bin/esidx" build "$ROOTDIR" -o "$DB"
fi

say "== 1/2 the privileged half: sfa-server as root, marked on $ROOTDIR"
$SUDO setsid nohup "$PREFIX/bin/sfa-server" "$ROOTDIR" --group "$GROUP" "$SOCKET" \
    >"$LOGDIR/sfa.log" 2>&1 </dev/null &
for _ in $(seq 50); do [ -S "$SOCKET" ] && break; sleep 0.2; done
[ -S "$SOCKET" ] || { say "   FAILED to start; see $LOGDIR/sfa.log"; exit 1; }
ls -l "$SOCK" | sed 's/^/   /'
say "   negotiated: $(grep -o 'mark 模式.*' "$LOGDIR/sfa.log" | head -1)"

say "== 2/2 the unprivileged half: esidx as $(id -un), watching it"
setsid nohup "$PREFIX/bin/esidx" -v 3 serve "$DB" -p "$PORT" --bind 127.0.0.1 \
    --watch="$SOCK" --sweep=3600 >"$LOGDIR/esidx.log" 2>&1 </dev/null &
for _ in $(seq 100); do grep -q 'esidx serving' "$LOGDIR/esidx.log" 2>/dev/null && break; sleep 0.2; done
grep -E 'esidx serving|esidx: watching' "$LOGDIR/esidx.log" | sed 's/^/   /'
grep -q 'esidx serving' "$LOGDIR/esidx.log" || { say "   FAILED to start; see $LOGDIR/esidx.log"; exit 1; }

say ""
say "running.  index server: 127.0.0.1:$PORT (no privilege)   proxy: root, socket $SOCKET"
say "logs:    $LOGDIR/{sfa,esidx}.log"
say "stop:    sudo pkill -x sfa-server; pkill -x esidx"
say "query:   nc 127.0.0.1 $PORT   (or any ETP client; see README)"