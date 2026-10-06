# esidx -- ext4 search index engine. Refs: ../ext4_index_engine_design.md
#
#   make            optimised *and* sanitiser builds of everything
#   make check      both suites against both builds -- the gate in AGENTS.md 3.1
#   make opt        only the optimised build
#   make dbg        only the sanitiser build
#   make install    install the esidx binary to $(BINDIR) (default $(PREFIX)/bin)
#   make dist       a tarball that builds on the target: both repositories, with the suites
#   make deb        a Debian package instead: binaries, two systemd units, /etc/default/esidx
#                   (make deb-verify unpacks it and runs what came out)
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
# Three translation units, not one, since the watcher grew embedded mode: libsfa.c is the
# client SDK, sfa_server.c is the proxy's library half (sfa_srv_*, issue #10) and sfa_probe.c
# is what that half calls to negotiate fanotify. All three are built with esidx's flags, for
# the reason below the object rules.
SFA_H    = sfa/sfa.h
SFA_SRCS = sfa/libsfa.c sfa/sfa_server.c sfa/sfa_probe.c
SFA_HDRS = sfa/sfa.h sfa/sfa_server.h sfa/sfa_probe.h
OBJS += $(SFA_SRCS:.c=.o)
HDRS += $(SFA_HDRS)

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

# The submodule's translation units, built with esidx's flags rather than sfa's Makefile
# so the sanitiser flavour covers them too -- a leak or a bad read in the SDK is as much this
# project's bug as one in its own code, since the SDK is linked into the binary.
# -U_GNU_SOURCE cancels the -D in CFLAGS: these files `#define _GNU_SOURCE` themselves, and
# gcc warns about redefining a glibc builtin macro.
$(SFA_SRCS:.c=.o): %.o: %.c $(SFA_HDRS)
	$(CC) $(CFLAGS) -Wno-builtin-macro-redefined -U_GNU_SOURCE -c $< -o $@

$(SFA_SRCS:.c=.dbg.o): %.dbg.o: %.c $(SFA_HDRS)
	$(CC) $(DBG_CFLAGS) -Wno-builtin-macro-redefined -U_GNU_SOURCE -c $< -o $@

# The Makefile is a prerequisite of every object, because a change to it is a change to how
# the code is compiled -- a new flag, a different -D, a source added to $(OBJS). Without
# this, `make` leaves a binary that was built by rules that no longer exist, and the only
# symptom is the suites' mtime guard refusing to measure it ("build did not run or did not
# finish"), which is true and much less obvious than a rebuild.
$(OBJS) $(DBG_OBJS): Makefile

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

# ---------------------------------------------------------------- Debian package
#
# A .deb is the *other* deployment story, and the tarball above is the first: `dist` ships
# sources and builds on the target, a .deb ships binaries and installs them. So this target
# installs what install.sh installs -- through install.sh's own Makefile targets, not a second
# file list written out here, because a file list that exists in two places is one that will
# differ in one of them (AGENTS.md 6, one decision per place).
#
# What the package adds over the tarball is the part a tarball cannot carry: systemd units and
# a maintainer script. The units exist in the repository now, and they are the two that were
# measured on r7000, minus the things that were only true of that machine (/home/wahaha, log
# files, default.target, one site's IP allow-list) and plus the things a package needs (a
# system user, a configuration file the unit reads, journald).
#
# What it deliberately does not do is start the services. See packaging/postinst: the index
# server refuses to run without a snapshot and building the first one is a full scan, which
# does not belong in a maintainer script.

DEB_NAME  := esidx
DEB_ARCH  := $(shell dpkg --print-architecture 2>/dev/null || echo unknown)
# dpkg's version field must begin with a digit and carry no whitespace. `git describe` falls
# back to the abbreviated hash when a repository has no tags, and a hash's first character is
# a letter about one time in sixteen -- so this is sanitised here rather than discovered from
# dpkg-deb's error, which arrives after everything else has been built.
DEB_VER_SAN := $(shell printf '%s' '$(VERSION)' | tr -c 'A-Za-z0-9.+~-' '-')
DEB_VER_FST := $(shell printf '%s' '$(DEB_VER_SAN)' | cut -c1)
DEB_VERSION := $(if $(filter 0 1 2 3 4 5 6 7 8 9,$(DEB_VER_FST)),$(DEB_VER_SAN),0+$(DEB_VER_SAN))
DEB        := $(DISTDIR)/$(DEB_NAME)_$(DEB_VERSION)_$(DEB_ARCH).deb
DEBSTAGE   := /tmp/.esidx-deb-$(DEB_VERSION)
# The maintainer is whoever configured the repository, because that is the only name there
# is; a checkout with no git identity gets a placeholder rather than a failing target.
DEB_MNAME  := $(shell $(GIT) config user.name 2>/dev/null || echo esidx)
DEB_MEMAIL := $(shell $(GIT) config user.email 2>/dev/null || echo root@localhost)

deb: require-sfa
	@command -v dpkg-deb >/dev/null 2>&1 || { \
	    echo "esidx: dpkg-deb not found -- a Debian package cannot be built without it." >&2; \
	    echo "       (this is a build-host tool; the target only needs dpkg to install)" >&2; \
	    exit 1; }
	@mkdir -p "$(DISTDIR)"
	@echo "== build (make opt: the same binary install.sh installs, not both flavours)"
	@$(MAKE) opt
	@echo "== stage into a Debian layout under $(DEBSTAGE)"
	@rm -rf "$(DEBSTAGE)"
	@mkdir -p "$(DEBSTAGE)/DEBIAN" "$(DEBSTAGE)/usr/bin" "$(DEBSTAGE)/usr/lib/systemd/system" \
	          "$(DEBSTAGE)/etc/default" "$(DEBSTAGE)/usr/share/doc/esidx"
	@$(MAKE) install PREFIX=/usr DESTDIR="$(DEBSTAGE)"
	@# sfa's install-bin, not its install: this is a runtime package, and dev headers and a
	@# static SDK archive belong to a -dev package that nothing here needs.
	@$(MAKE) -C sfa install-bin PREFIX=/usr DESTDIR="$(DEBSTAGE)"
	@install -m 0644 packaging/systemd/esidx.service \
	    "$(DEBSTAGE)/usr/lib/systemd/system/esidx.service"
	@install -m 0644 packaging/esidx.default "$(DEBSTAGE)/etc/default/esidx"
	@install -m 0644 README.md "$(DEBSTAGE)/usr/share/doc/esidx/README.md"
	@install -m 0644 docs/design.md "$(DEBSTAGE)/usr/share/doc/esidx/design.md"
	@install -m 0644 docs/everything-syntax.md \
	    "$(DEBSTAGE)/usr/share/doc/esidx/everything-syntax.md"
	@echo "== control metadata"
	@# Depends is one libc line, written out, rather than ${shlibs:Depends}: the build is
	@# C11 plus libc by decision D5, so there is no shared object to load and no library to
	@# name, and ${shlibs:Depends} would answer it by asking dpkg-shlibdeps -- which wants a
	@# debian/rules and a build-dependency closure, a lot of machinery to derive this. The
	@# comment that used to sit here explaining that was itself the first bug in this
	@# target: a DEBIAN/control file is RFC822 and has no comment syntax, so dpkg-deb
	@# rejected it with "field name '#' must be followed by colon". Hence this comment.
	@size=$$(du -ks --exclude=DEBIAN "$(DEBSTAGE)" | cut -f1); \
	 sed -e 's|@VERSION@|$(DEB_VERSION)|' -e 's|@ARCH@|$(DEB_ARCH)|' \
	     -e 's|@SIZE@|'"$$size"'|' -e '/^Maintainer: @MAINTAINER@$$/d' \
	     packaging/control.in > "$(DEBSTAGE)/DEBIAN/control"; \
	 printf 'Maintainer: %s <%s>\n' "$(DEB_MNAME)" "$(DEB_MEMAIL)" \
	     >> "$(DEBSTAGE)/DEBIAN/control"
	@printf '%s\n' '/etc/default/esidx' > "$(DEBSTAGE)/DEBIAN/conffiles"
	@# dpkg-deb only *warns* about a control file with no Maintainer, and then builds the
	@# package anyway -- which is how the previous version of this target shipped a control
	@# file whose Maintainer had been appended onto the last line of the description: the
	@# template had no final newline and `>>` concatenates. Assert it here instead of
	@# trusting the warning, and assert the thing that warning was about.
	@grep -q '^Maintainer: .* <.*>$$' "$(DEBSTAGE)/DEBIAN/control" || { \
	    echo "esidx: no Maintainer line in the generated control file -- refusing to build." >&2; \
	    exit 1; }
	@# Maintainer scripts go in with their CRs stripped. A CRLF in postinst is not a style
	@# question: dpkg runs it with /bin/sh, and a stray CR lands in the middle of a command.
	@# The tree is developed on a filesystem where this happens silently, so it is done here
	@# rather than trusted (AGENTS.md 6.2).
	@for s in postinst prerm postrm; do \
	    sed 's/\r$$//' "packaging/$$s" > "$(DEBSTAGE)/DEBIAN/$$s"; \
	    chmod 0755 "$(DEBSTAGE)/DEBIAN/$$s"; \
	done
	@echo "== build the package (--root-owner-group: dpkg must not record this user's uid)"
	@dpkg-deb --build --root-owner-group "$(DEBSTAGE)" "$(DEB)" >/dev/null
	@rm -rf "$(DEBSTAGE)"

# A package that cannot be read is not a package, and the two ways this goes wrong are both
# invisible in a successful `dpkg-deb --build`: an empty DEBIAN directory, and a binary that
# was staged from somewhere else. So this unpacks it and *runs what came out* -- builds a
# snapshot with the packaged server and probes with the packaged proxy -- rather than checking
# that dpkg-deb exited 0. The units are parsed too, because a unit that does not parse fails
# at boot, on someone else's machine, with the install long since reported as successful.
deb-verify: deb
	@rm -rf /tmp/.esidx-debcheck /tmp/.esidx-debcheck-fixture
	@mkdir -p /tmp/.esidx-debcheck /tmp/.esidx-debcheck-fixture/sub
	@printf 'alpha\n' > /tmp/.esidx-debcheck-fixture/alpha.txt
	@printf 'beta\n'  > /tmp/.esidx-debcheck-fixture/sub/beta.conf
	@dpkg-deb --extract "$(DEB)" /tmp/.esidx-debcheck
	@# -x takes the data archive only; the control files come out with -e. Asking for
	@# DEBIAN/conffiles under -x finds nothing, and reads like a package that declared no
	@# conffile -- which is a different (and wrong) conclusion.
	@dpkg-deb --control "$(DEB)" /tmp/.esidx-debcheck/DEBIAN
	@echo "== contents"
	@dpkg-deb --contents "$(DEB)" | awk '{print "   " $$NF}' | sort | sed 's|/tmp/.esidx-debcheck||'
	@# The unit is one file now, and the check that it is *the* unit the package ships is the
	@# one that would catch a rename nobody propagated: the old pair must not be in the package
	@# at all, because a leftover proxy unit would start a second watcher beside the embedded
	@# one and answer from a socket the server no longer reads.
	@for f in usr/bin/esidx usr/bin/sfa-server \
	          usr/lib/systemd/system/esidx.service \
	          etc/default/esidx DEBIAN/conffiles; do \
	    test -e "/tmp/.esidx-debcheck/$$f" \
	        || { echo "   MISSING: $$f"; exit 1; }; \
	done
	@for f in usr/lib/systemd/system/esidx-proxy.service \
	          usr/lib/systemd/system/esidx-index.service; do \
	    test -e "/tmp/.esidx-debcheck/$$f" \
	        && { echo "   STALE: $$f is still in the package"; exit 1; }; \
	done; true
	@test -x /tmp/.esidx-debcheck/usr/bin/esidx \
	    || { echo "   usr/bin/esidx is not executable"; exit 1; }
	@grep -q '^/etc/default/esidx$$' /tmp/.esidx-debcheck/DEBIAN/conffiles \
	    || { echo "   /etc/default/esidx is not a conffile, so an upgrade may overwrite it"; exit 1; }
	@# Counted with tr rather than grepped for $'\r': make runs recipes with /bin/sh, which on
	@# Debian is dash, and dash has no ANSI-C quoting -- so `$'\r'` reaches grep as the
	@# literal characters and the pattern matches the file that has no CR in it at all. The
	@# first version of this check passed on a clean script and failed on a clean script.
	@for s in postinst prerm postrm; do \
	    cr=$$(tr -cd '\r' < "/tmp/.esidx-debcheck/DEBIAN/$$s" | wc -c); \
	    if [ "$$cr" != 0 ]; then \
	        echo "   DEBIAN/$$s has CRLF line endings -- dpkg would run it with stray CRs"; \
	        exit 1; \
	    fi; \
	done
	@echo "== the packaged server builds a snapshot (proves it runs and the SDK is linked in)"
	@/tmp/.esidx-debcheck/usr/bin/esidx build /tmp/.esidx-debcheck-fixture \
	    -o /tmp/.esidx-debcheck/t.idx 2>&1 | tail -2 | sed 's/^/   /'
	@test -s /tmp/.esidx-debcheck/t.idx || { echo "   no snapshot was written"; exit 1; }
	@echo "== the packaged proxy probes fanotify"
	@/tmp/.esidx-debcheck/usr/bin/sfa-server --probe / 2>&1 | head -3 | sed 's/^/   /'
	@echo "== the units name binaries this package actually ships"
	@# systemd-analyze verify resolves ExecStart against the real filesystem root, and the
	@# units are being checked outside their install prefix, so it reports every command as
	@# missing -- and `--root` is not an option for `verify` before systemd 252. So the
	@# joining is checked here instead, which is the half that can actually be wrong: a unit
	@# naming /usr/local/bin/esidx while the package installs /usr/bin/esidx is a valid unit
	@# and a broken deployment, and nothing else in this target would notice.
	@for u in esidx; do \
	    unit="/tmp/.esidx-debcheck/usr/lib/systemd/system/$$u.service"; \
	    cmd=$$(sed -n 's/^ExecStart=//p' "$$unit" | head -1 | awk '{print $$1}'); \
	    case "$$cmd" in /*) ;; *) echo "   $$u: ExecStart does not start with a path: $$cmd"; \
	        exit 1 ;; esac; \
	    if [ ! -x "/tmp/.esidx-debcheck$$cmd" ]; then \
	        echo "   $$u: ExecStart runs $$cmd, which this package does not install" >&2; \
	        exit 1; \
	    fi; \
	    echo "   $$u -> $$cmd"; \
	done
	@echo "== the unit parse"
	@# Anything systemd complains about beyond the missing-command lines is a real defect,
	@# so those are the only lines filtered out. An unrelated unit on the host (snapd) is not
	@# this package's business and is left out of the judgement entirely.
	@if command -v systemd-analyze >/dev/null 2>&1; then \
	    out=$$(systemd-analyze verify \
	            /tmp/.esidx-debcheck/usr/lib/systemd/system/esidx.service 2>&1 || true); \
	    bad=$$(printf '%s\n' "$$out" | grep 'esidx' | grep -v 'is not executable' || true); \
	    if [ -n "$$bad" ]; then \
	        printf '%s\n' "$$bad" | sed 's/^/   /'; \
	        echo "   the packaged unit produced diagnostics -- see above"; exit 1; \
	    fi; \
	    echo "   the unit parses"; \
	else echo "   (systemd-analyze not present; skipped)"; fi
	@rm -rf /tmp/.esidx-debcheck /tmp/.esidx-debcheck-fixture
	@echo "deb-verify: readable, runs, snapshot builds, proxy probes, the unit parses"

distclean: clean
	rm -rf $(DISTDIR) /tmp/.esidx-stage-esidx-* /tmp/.esidx-deb-* /tmp/.esidx-debcheck* /tmp/.dv-*

# `make DEBUG=1` means nothing here and is gone on purpose; `make opt`, `make dbg` and
# `make check` are the spellings.

clean:
	rm -f $(OBJS) $(DBG_OBJS) esidx esidx-dbg etp-probe etp-probe-dbg order-ref order-ref-dbg

.PHONY: require-sfa all opt dbg sfa-proxy check test test-etp test-all install uninstall clean dist dist-verify deb deb-verify distclean
