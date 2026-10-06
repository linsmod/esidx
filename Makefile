# esidx -- ext4 search index engine. Refs: ../ext4_index_engine_design.md
#
#   make            optimised *and* sanitiser builds of everything
#   make check      both suites against both builds -- the gate in AGENTS.md 3.1
#   make opt        only the optimised build
#   make dbg        only the sanitiser build
#   make install    install the esidx binary to $(BINDIR) (default $(PREFIX)/bin)
#
# One `make` builds both flavours, and there is no variable that changes that -- the gate
# in AGENTS.md 3.1 needs both, and a selector that could point it at one of them is how it
# was going wrong. `make check` runs the whole gate.
#
# What this replaced: the two flavours shared object names, so switching meant
# `make clean && make DEBUG=1`, and forgetting the clean left the -O0+ASan run using -O2
# objects -- a run that looked sanitised and was not, which cost an hour of "the ASan
# failure is not reproducible". The objects are named apart now (%.o and %.dbg.o), so the
# two coexist and neither can be stale. There is no DEBUG variable: `make` means both,
# `make opt` and `make dbg` name one, and the suites choose with ESIDX_BUILD.
#
# Logging is switched at runtime with ESIDX_LOG=<level> or -v N; the sanitiser build
# only changes the default level (log.c reads ESIDX_DEBUG for exactly that).
#
# install honours the usual PREFIX/DESTDIR pair: PREFIX picks the tree
# (default /usr/local), DESTDIR is prepended for staged packaging. Only the
# server binary is installed; etp-probe and order-ref are test peers
# (AGENTS.md 1.4, 3.2), not user-facing tools.
#
# The suites pick a flavour with ESIDX_BUILD=dbg, or a single binary with ESIDX_BIN.

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

# -O0 so the sanitiser's own diagnostics get usable line numbers; D7's ABI is unchanged,
# so this build exercises the same code as the optimised one.
DBG_CFLAGS  = -std=c11 -O0 -g -Wall -Wextra -D_GNU_SOURCE -DESIDX_DEBUG \
              -fsanitize=address,undefined -fno-omit-frame-pointer
DBG_LDFLAGS = -fsanitize=address,undefined

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INSTALL ?= install

HDRS = esidx.h syntax.h etp.h log.h timer.h lexer.h watch.h

# storage + index (design §4, §5) | syntax (§6.1) | execution (§6.3) | protocol (§1)
# | watcher (watch.c is in the collection layer with scan.c: it marks directories)
OBJS = store.o index.o scan.o trigram.o lexer.o parser.o regex.o query.o log.o etp.o watch.o main.o
DBG_OBJS = $(OBJS:.o=.dbg.o)

# The watcher's client SDK comes from sfa, a submodule in its own repository, and it is
# optional on purpose. Without it the binary still builds, `--watch` says the feature is
# not built in, and every other command is unaffected -- which is the right behaviour for
# a checkout nobody cloned a submodule into. It is also why the dependency is a wildcard
# rather than a plain name: a missing file must not stop the build before the rule runs.
SFA_H   = sfa/sfa.h
SFA_LIB = sfa/libsfa.c
ifneq ($(wildcard $(SFA_H)),)
OBJS   += sfa/libsfa.o
SFA_DEF = -DESIDX_HAVE_SFA=1
HDRS   += $(SFA_H)
endif
CFLAGS    += $(SFA_DEF)
DBG_CFLAGS += $(SFA_DEF)

# `all` is etp-probe as well as the server, so one `make` leaves a checkout ready
# for both suites. It used to be two steps (AGENTS.md 3.1), and the second one was
# easy to forget: test_etp.sh then stops with "./etp-probe not built", which reads
# like a broken checkout rather than a missing prerequisite. The probe is one
# translation unit and ~430 lines, so building it by default costs nothing next to
# the server itself.
all: opt dbg sfa-proxy

# The privileged proxy the watcher talks to, built here for the same reason etp-probe and
# order-ref are: test_watch.sh stopping with "./sfa/sfa-server not built" reads like a
# broken checkout rather than a forgotten second step (AGENTS.md 3.1). It lives in the
# submodule and has its own Makefile, so this only asks for the binary -- if the submodule
# was never cloned there is nothing to ask for and the watcher suite skips itself.
sfa-proxy:
	@if [ -f sfa/Makefile ]; then $(MAKE) -C sfa sfa-server; \
	  else echo "sfa/ not present -- the watcher is not available in this checkout"; fi

opt: esidx etp-probe order-ref
dbg: esidx-dbg etp-probe-dbg order-ref-dbg

esidx: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

esidx-dbg: $(DBG_OBJS)
	$(CC) $(DBG_CFLAGS) -o $@ $(DBG_OBJS) $(DBG_LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

# The pattern rule is spelled out rather than shared with %.o: one CFLAGS cannot hold two
# sets of flags, and a target-specific variable on a pattern rule works but reads as magic.
%.dbg.o: %.c $(HDRS)
	$(CC) $(DBG_CFLAGS) -c $< -o $@

# The submodule's one translation unit, built with esidx's flags rather than sfa's Makefile
# so the sanitiser flavour covers it too -- a leak or a bad read in the SDK is as much this
# project's bug as one in its own code, since the SDK is linked into the binary.
sfa/libsfa.o: $(SFA_LIB) $(SFA_H)
	$(CC) $(CFLAGS) -Wno-builtin-macro-redefined -U_GNU_SOURCE -c $< -o $@

sfa/libsfa.dbg.o: $(SFA_LIB) $(SFA_H)
	$(CC) $(DBG_CFLAGS) -Wno-builtin-macro-redefined -U_GNU_SOURCE -c $< -o $@

# The gate, in the order AGENTS.md 3.1 asks for: the index suite before the protocol one,
# so a parse regression is not read as a protocol fault, and both builds of each.
check: all
	@echo "== optimised: index suite"
	@./test.sh
	@echo "== optimised: protocol suite"
	@./test_etp.sh
	@echo "== sanitiser: index suite"
	@ESIDX_BUILD=dbg ASAN_OPTIONS=detect_leaks=1 ./test.sh
	@echo "== sanitiser: protocol suite"
	@ESIDX_BUILD=dbg ASAN_OPTIONS=detect_leaks=1 ./test_etp.sh
	@echo "== both suites, both builds: green"

test: opt
	./test.sh

# protocol acceptance, driven over a real socket by tools/etp_probe.c, which
# applies a real client's parsing rules. AGENTS.md 1.4 covers the second peer --
# Everything itself, pointed at this server.
etp-probe: tools/etp_probe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

etp-probe-dbg: tools/etp_probe.c
	$(CC) $(DBG_CFLAGS) -o $@ $< $(DBG_LDFLAGS)

# The sort-order oracle for test.sh, and the reason it is a program rather than a
# pipeline: `sort -f` is not a byte order and neither is `tr | sort`. See the header.
order-ref: tools/order_ref.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

order-ref-dbg: tools/order_ref.c
	$(CC) $(DBG_CFLAGS) -o $@ $< $(DBG_LDFLAGS)

test-etp: opt
	./test_etp.sh

# both suites against the optimised build only; `check` is the real gate
test-all: test test-etp

install: esidx
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 esidx "$(DESTDIR)$(BINDIR)/esidx"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/esidx"

# `make DEBUG=1` means nothing here and is gone on purpose; `make opt`, `make dbg` and
# `make check` are the spellings.

clean:
	rm -f $(OBJS) $(DBG_OBJS) esidx esidx-dbg etp-probe etp-probe-dbg order-ref order-ref-dbg

.PHONY: all opt dbg sfa-proxy check test test-etp test-all install uninstall clean
