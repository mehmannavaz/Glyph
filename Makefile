# Makefile for Glyph
#
# Build:   make
# Test:    make test
# Install: make install
# Clean:   make clean
#
# The compiler is split into Unix-style modules with strict boundaries.
# Each .c file owns its concern. See src/glyph.h for the API.

CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wno-unused-function -Wno-unused-parameter -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  ?= -lm -ldl

# Source files — order doesn't matter, but grouped by concern
SRC = \
    src/lex.c     \
    src/parse.c   \
    src/ast.c     \
    src/value.c   \
    src/util.c    \
    src/interp.c  \
    src/irgen.c   \
    src/jit.c     \
    src/ffi.c     \
    src/stdlib.c  \
    src/json.c    \
    src/main.c

OBJ = $(SRC:.c=.o)

# Optional X11 IDE
ifeq ($(shell pkg-config --exists x11 && echo yes),yes)
    IDE_SRC = src/glyphide.c
    IDE_CFLAGS = $(shell pkg-config --cflags x11)
    IDE_LDLIBS = $(shell pkg-config --libs x11)
endif

all: glyph glyphide

glyph: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

glyphide: $(IDE_SRC:.c=.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(IDE_SRC:.c=.o) $(IDE_CFLAGS) $(IDE_LDLIBS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(IDE_CFLAGS) -c -o $@ $<

test: glyph
	@echo "Running test suite..."
	@pass=0; fail=0; failed=""; \
	for t in tests/*.glyph; do \
	    name=$$(basename "$$t" .glyph); \
	    expected="tests/$$name.expected"; \
	    if [ -f "$$expected" ]; then \
		actual=$$(GLYPH_LIB=$(CURDIR)/lib timeout 15 ./glyph "$$t" 2>&1); \
		expc=$$(cat "$$expected"); \
		if [ "$$actual" = "$$expc" ]; then \
		    pass=$$((pass+1)); \
		else \
		    fail=$$((fail+1)); failed="$$failed $$name"; \
		fi; \
	    fi; \
	done; \
	echo "PASS: $$pass"; echo "FAIL: $$fail"; \
	if [ -n "$$failed" ]; then echo "Failed:$$failed"; fi; \
	[ $$fail -eq 0 ]

install: glyph
	cp glyph $(DESTDIR)/usr/local/bin/glyph
	cp -r lib $(DESTDIR)/usr/local/lib/glyph
	cp man/glyphide.1 $(DESTDIR)/usr/local/share/man/man1/ 2>/dev/null || true

clean:
	rm -f glyph glyphide $(OBJ) $(IDE_SRC:.c=.o)

.PHONY: all test install clean
