# esidx -- ext4 search index engine. Refs: ../ext4_index_engine_design.md
#
#   make            optimised build, log level = warn
#   make DEBUG=1    -O0 -g -fsanitize=address,undefined, log level = debug
#   make test       build + ./test.sh
#
# Logging is switched at runtime with ESIDX_LOG=<level> or -v N; DEBUG=1 only
# changes the default.

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?=

ifeq ($(DEBUG),1)
CFLAGS  += -O0 -g -DESIDX_DEBUG -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

# storage + index (design §4, §5) | syntax (§6.1) | execution (§6.3) | protocol (§1)
OBJS = store.o index.o scan.o lexer.o parser.o regex.o query.o log.o etp.o main.o

all: esidx

esidx: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c esidx.h syntax.h etp.h log.h timer.h lexer.h
	$(CC) $(CFLAGS) -c $< -o $@

test: esidx
	./test.sh

# protocol acceptance, driven over a real socket by a transcription of the
# ETP client's own parsing rules (tools/etp_probe.c)
etp-probe: tools/etp_probe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test-etp: esidx etp-probe
	./test_etp.sh

# both suites; the index/language one first so a regression there is obvious
# before the protocol layer is blamed
test-all: test test-etp

clean:
	rm -f $(OBJS) esidx etp-probe

.PHONY: all test test-etp test-all clean
