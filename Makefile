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

# The watcher's client SDK comes from sfa, a submodule in its own repository. It is a
# DEPENDENCY, not an option, and the build says so rather than working around it: the
# pointer is committed in the tree, so "sfa is not there" is a checkout nobody finished
# (`git clone` without `--recurse-submodules`, or a `git submodule update` never run), not a
# configuration to support. Building anyway would hand back a server with no watcher, no
# loss signal and no sweep-on-loss, and nothing about it would say so until someone passed
# `--watch` and read an error -- which is the failure mode this project keeps arguing
# against (AGENTS.md 6, failures are visible).
SFA_H   = sfa/sfa.h
SFA_LIB = sfa/libsfa.c
OBJS += sfa/libsfa.o
HDRS += $(SFA_H)

# `all` is etp-probe as well as the server, so one `make` leaves a checkout ready
# for both suites. It used to be two steps (AGENTS.md 3.1), and the second one was
# easy to forget: test_etp.sh then stops with "./etp-probe not built", which reads
# like a broken checkout rather than a missing prerequisite. The probe is one
# translation unit and ~430 lines, so building it by default costs nothing next to
# the server itself.
#
# .DEFAULT_GOAL is named rather than left to be the first target in the file, because
# that is a silent trap: adding a rule above `all` -- which is what require-sfa below
# was -- turns a bare `make` into a no-op that still exits 0. It cost a deployment to
# find (r7000: `make -j16` returned in 12 ms having built nothing) and it is invisible
# to anyone who checks for errors instead of for the artefact.
.DEFAULT_GOAL := all

all: opt dbg sfa-proxy

# Checked in a recipe rather than with $(error) so that `make clean` still works on a
# checkout that has not finished initialising: everything that compiles depends on this,
# and clean is the one thing that must not need the dependency to be present.
require-sfa:
	@if [ ! -f "$(SFA_H)" ]; then \
	    echo "esidx: $(SFA_H) is missing: the sfa submodule is not checked out." >&2; \
	    echo "       run: git submodule update --init --recursive" >&2; \
	    exit 1; \
	fi

# The privileged proxy the watcher talks to, built here for the same reason etp-probe and
# order-ref are: test_watch.sh stopping with "./sfa/sfa-server not built" reads like a
# broken checkout rather than a forgotten second step (AGENTS.md 3.1). It lives in the
# submodule and has its own Makefile; the error above has already established that sfa/ is
# present, so this is only asking for the binary.
sfa-proxy:
	@$(MAKE) -C sfa sfa-server

opt: require-sfa esidx etp-probe order-ref
dbg: require-sfa esidx-dbg etp-probe-dbg order-ref-dbg

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

# The Makefile is a prerequisite of every object, because a change to it is a change to how
# the code is compiled -- a new flag, a different -D, a source added to $(OBJS). Without
# this, `make` leaves a binary that was built by rules that no longer exist, and the only
# symptom is the suites' mtime guard refusing to measure it ("build did not run or did not
# finish"), which is true and much less obvious than a rebuild.
$(OBJS) $(DBG_OBJS) sfa/libsfa.o sfa/libsfa.dbg.o: Makefile

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

# ---------------------------------------------------------------- deployment package
#
# One tarball that is the whole deployment: esidx's sources, the sfa submodule's sources,
# and install.sh. The submodule is the reason this target exists rather than a `tar czf` --
# a gitlink is not a directory as far as tar is concerned, and `tar czf x.tar.gz sfa/`
# either follows into .git or fails, which is what a hand-rolled deployment package does.
# It is also the only way to ship the *suites* with the thing: a binary-only tarball cannot
# be re-verified on the machine it lands on, and "it builds on its own" is the only
# practical test that a package is self-contained (README, Independence).
#
# The file list comes from `git ls-files` on both repositories rather than from a wildcard,
# for two reasons that are one reason: a wildcard would pick up build artefacts and the
# untracked, and a package's contents must be exactly what the commit says. An empty
# sfa/ is a hard error above, so the list cannot come out half-empty here.
GIT       ?= git
REVISION  := $(shell $(GIT) rev-parse --short=12 HEAD 2>/dev/null)
DESCRIBE  := $(shell $(GIT) describe --tags --always 2>/dev/null)
# `git status --porcelain` rather than `describe --dirty`: the latter compares tracked
# files only, so untracked ones (install.sh before it is committed, docs/todo.md always)
# would change the package without changing the version string.
DIRTY     := $(shell test -z "$$($(GIT) status --porcelain 2>/dev/null)" || echo -dirty)
VERSION   := $(if $(DESCRIBE),$(DESCRIBE),$(REVISION))$(DIRTY)
MTIME     := $(shell $(GIT) show -s --format=%ct HEAD 2>/dev/null || echo 0)
# Resolved here rather than in the recipe: the recipe writes VERSION-SOURCES inside single
# quotes, so a `$$(git ...)` in it reaches the file as literal text instead of the hash.
SFA_REV   := $(shell $(GIT) -C sfa rev-parse --short=12 HEAD 2>/dev/null || echo unknown)

DISTDIR   := dist
PKGNAME   := esidx-$(VERSION)
# Staged outside the build tree on purpose: the build tree may be on a 9p/v9fs mount,
# where chmod is a no-op, and then every file in the package lands as 0777 -- sources and
# documentation included. The mode a file gets in the package has to come from git, and the
# only way to apply it is on a filesystem that lets us.
STAGE     := /tmp/.esidx-stage-$(PKGNAME)
PKG       := $(DISTDIR)/$(PKGNAME).tar.gz

dist: require-sfa
	@mkdir -p "$(DISTDIR)"
	@$(GIT) ls-files -s | grep -v '	sfa$$' > "$(DISTDIR)/.esidx-files"
	@$(GIT) -C sfa ls-files -s > "$(DISTDIR)/.sfa-files"
	@rm -rf "$(STAGE)"
	@mkdir -p "$(STAGE)/$(PKGNAME)"
	@# mode, hash, stage, path -> the mode is git's, not the staging filesystem's. Copying
	@# with cp would carry whatever the build tree happens to say, and on a 9p/v9fs mount
	@# (this project is developed on one) that is 0777 for every file in the package,
	@# sources and documentation included. A package's contents are what the commit says.
	@while read -r mode _ _ f; do \
	    mkdir -p "$(STAGE)/$(PKGNAME)/$$(dirname "$$f")"; \
	    cp "$$f" "$(STAGE)/$(PKGNAME)/$$f"; \
	    chmod 0755 "$(STAGE)/$(PKGNAME)/$$f" 2>/dev/null || true; \
	    [ "$$mode" = 100644 ] && chmod 0644 "$(STAGE)/$(PKGNAME)/$$f"; \
	done < "$(DISTDIR)/.esidx-files"
	@while read -r mode _ _ f; do \
	    mkdir -p "$(STAGE)/$(PKGNAME)/sfa/$$(dirname "$$f")"; \
	    cp "sfa/$$f" "$(STAGE)/$(PKGNAME)/sfa/$$f"; \
	    chmod 0755 "$(STAGE)/$(PKGNAME)/sfa/$$f" 2>/dev/null || true; \
	    [ "$$mode" = 100644 ] && chmod 0644 "$(STAGE)/$(PKGNAME)/sfa/$$f"; \
	done < "$(DISTDIR)/.sfa-files"
	@printf '%s\n' '$(VERSION)' > "$(STAGE)/$(PKGNAME)/VERSION"
	@printf '%s\n' 'esidx $(REVISION) + sfa $(SFA_REV)' \
	    > "$(STAGE)/$(PKGNAME)/VERSION-SOURCES"
	@chmod 0644 "$(STAGE)/$(PKGNAME)/VERSION" "$(STAGE)/$(PKGNAME)/VERSION-SOURCES"
	@tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@$(MTIME)' \
	     -C "$(STAGE)" -czf "$(PKG)" "$(PKGNAME)"
	@rm -rf "$(STAGE)" "$(DISTDIR)/.esidx-files" "$(DISTDIR)/.sfa-files"
	@echo "package : $(PKG)"
	@echo "version : $(VERSION)"
	@echo "files   : $$(tar -tzf "$(PKG)" | grep -vc '/$$')"
	@cd "$(DISTDIR)" && sha256sum "$(PKGNAME).tar.gz" > "$(PKGNAME).tar.gz.sha256" && \
	    cat "$(PKGNAME).tar.gz.sha256"

# Prove the package is self-contained by unpacking it somewhere else and building it there,
# and that it is *faithful*: the file list and the modes have to be what the two commits say,
# because the two ways this can go wrong are both invisible in a tarball that merely opens.
# A file missing from the package fails when somebody unpacks it on another machine; a file
# with the wrong mode fails more quietly, as an executable documentation file or a
# non-executable install.sh.
dist-verify: dist
	@rm -rf "$(STAGE)" && mkdir -p "$(STAGE)"
	@tar -xzf "$(PKG)" -C "$(STAGE)"
	@echo "== contents: the package against the two commits"
	@cd "$(STAGE)/$(PKGNAME)" && { \
	    { $(GIT) -C "$(CURDIR)" ls-files | grep -v '^sfa$$'; \
	      $(GIT) -C "$(CURDIR)/sfa" ls-files | sed 's|^|sfa/|'; \
	      printf 'VERSION\nVERSION-SOURCES\n'; } | LC_ALL=C sort > /tmp/.dv-want; \
	    find . -type f | sed 's|^\./||' | LC_ALL=C sort > /tmp/.dv-have; \
	    if diff -u /tmp/.dv-want /tmp/.dv-have; then \
	        echo "   file list matches ($$(wc -l < /tmp/.dv-have) files)"; \
	    else echo "   FILE LIST DIFFERS"; exit 1; fi; }
	@echo "== modes: nothing executable that git says is not, nothing missing that it says is"
	@cd "$(STAGE)/$(PKGNAME)" && $(GIT) -C "$(CURDIR)" ls-files -s \
	    | grep -v '	sfa$$' > /tmp/.dv-modes && \
	    $(GIT) -C "$(CURDIR)/sfa" ls-files -s | sed 's|^|sfa/|' >> /tmp/.dv-modes && \
	    bad=0; \
	    while read -r mode _ _ f; do \
	        case "$$mode" in 100644) want=644 ;; 100755) want=755 ;; *) want="" ;; esac; \
	        [ -n "$$want" ] || continue; \
	        have=$$(stat -c %a "$$f" 2>/dev/null || echo missing); \
	        if [ "$$have" != "$$want" ]; then \
	            echo "   mode $$f: want $$want have $$have"; bad=1; \
	        fi; \
	    done < /tmp/.dv-modes; \
	    [ "$$bad" = 0 ] && echo "   every mode matches" || { echo "   MODES DIFFER"; exit 1; }
	@echo "== building the unpacked package"
	@$(MAKE) -C "$(STAGE)/$(PKGNAME)" -j4 >/dev/null && \
	    echo "   build ok" || { echo "   BUILD FAILED"; exit 1; }
	@echo "== the watcher suite, run from the unpacked package"
	@cd "$(STAGE)/$(PKGNAME)" && (./test_watch.sh > /tmp/.dv-watch.log 2>&1; \
	    echo $$? > /tmp/.dv-watch.rc) || true
	@tail -3 /tmp/.dv-watch.log | sed 's/^/   /'
	@test "$$(cat /tmp/.dv-watch.rc)" = 0 || { echo "   THE SUITE FAILED -- see /tmp/.dv-watch.log"; exit 1; }
	@rm -rf "$(STAGE)" /tmp/.dv-want /tmp/.dv-have /tmp/.dv-modes /tmp/.dv-watch.log /tmp/.dv-watch.rc
	@echo "dist-verify: faithful to both commits, builds on its own, watcher suite passes"

distclean: clean
	rm -rf $(DISTDIR) /tmp/.esidx-stage-esidx-* /tmp/.dv-*

# `make DEBUG=1` means nothing here and is gone on purpose; `make opt`, `make dbg` and
# `make check` are the spellings.

clean:
	rm -f $(OBJS) $(DBG_OBJS) esidx esidx-dbg etp-probe etp-probe-dbg order-ref order-ref-dbg

.PHONY: require-sfa all opt dbg sfa-proxy check test test-etp test-all install uninstall clean dist dist-verify distclean
