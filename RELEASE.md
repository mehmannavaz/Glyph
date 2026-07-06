# Glyph v0.2.0 Release

## Quick Start

```bash
# Run a program
./bin/glyph tests/01-hello.glyph

# Start the REPL
./bin/glyph repl

# Lint a file
./bin/glyph lint tests/06-fib.glyph

# Start the IDE (requires X11)
./bin/glyphide tests/17-fizzbuzz.glyph
```

## What's Included

```
bin/        Pre-built binaries (glyph, glyphide, screenshot)
src/        Full source code (C11, cross-platform)
lib/        X11 bindings library (pure Glyph)
tests/      24 test programs (all passing)
examples/   7 example programs including X11 GUI
docs/       Language spec, tutorial, stdlib ref, IDE docs, BUILD.md, SELFHOST.md, PROBLEMS.md
man/        Man pages for glyph(1) and glyphide(1)
selfhost/   Self-hosting compiler (Glyph written in Glyph)
Makefile    Cross-platform build (Linux, macOS, Windows/MinGW)
build.sh    Unix build script
build.bat   Windows build script
```

## Build from Source

```bash
make            # builds glyph + glyphide
make test       # run 24-test suite
make install    # install to /usr/local (Linux)
```

## Platforms

| Platform | Status |
|----------|--------|
| Linux    | Full support |
| macOS    | Full support |
| Windows  | Full support (MinGW) |
