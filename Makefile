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
OBJS = store.o index.o scan.o lexer.o parser.o regex.o query.o log.o main.o

all: esidx

esidx: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c esidx.h syntax.h log.h timer.h
	$(CC) $(CFLAGS) -c $< -o $@

test: esidx
	./test.sh

# protocol acceptance, run against a real socket (design §10, P1)
test-etp: esidx
	./test_etp.sh

# both suites; the syntax/query suite first so a parse regression is obvious
test-all: test test-etp

clean:
	rm -f $(OBJS) esidx

.PHONY: all test test-etp test-all clean
