# esidx -- ext4 search index engine. Refs: ../ext4_index_engine_design.md
#
#   make            optimised build, log level = warn
#   make DEBUG=1    -O0 -g -fsanitize=address,undefined, log level = debug
#   make test       build + ./test.sh
#   make install    install the esidx binary to $(BINDIR) (default $(PREFIX)/bin)
#
# Logging is switched at runtime with ESIDX_LOG=<level> or -v N; DEBUG=1 only
# changes the default.
#
# install honours the usual PREFIX/DESTDIR pair: PREFIX picks the tree
# (default /usr/local), DESTDIR is prepended for staged packaging. Only the
# esidx binary is installed; etp-probe is a test peer (AGENTS.md 1.4), not a
# user-facing tool.

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INSTALL ?= install

ifeq ($(DEBUG),1)
CFLAGS  += -O0 -g -DESIDX_DEBUG -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

# storage + index (design §4, §5) | syntax (§6.1) | execution (§6.3) | protocol (§1)
OBJS = store.o index.o scan.o trigram.o lexer.o parser.o regex.o query.o log.o etp.o main.o

all: esidx

esidx: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c esidx.h syntax.h etp.h log.h timer.h lexer.h
	$(CC) $(CFLAGS) -c $< -o $@

test: esidx
	./test.sh

# protocol acceptance, driven over a real socket by tools/etp_probe.c, which
# applies a real client's parsing rules. AGENTS.md 1.4 covers the second peer --
# Everything itself, pointed at this server.
etp-probe: tools/etp_probe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test-etp: esidx etp-probe
	./test_etp.sh

# both suites; the index/language one first so a regression there is obvious
# before the protocol layer is blamed
test-all: test test-etp

install: esidx
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 esidx "$(DESTDIR)$(BINDIR)/esidx"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/esidx"

clean:
	rm -f $(OBJS) esidx etp-probe

.PHONY: all test test-etp test-all install uninstall clean
