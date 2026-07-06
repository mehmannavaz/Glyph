# Glyph Linux Release

## Quick Start (Linux)

```bash
./glyph tests/01-hello.glyph
```

Output:
```
Hello, World!
```

## What's Included

- `glyph` — interpreter, compiler, linter, REPL (306 KB)
- `glyphide` — X11 graphical IDE (64 KB)

## Usage

```bash
# Run a program
./glyph tests/01-hello.glyph

# Start the REPL
./glyph repl

# Lint a file
./glyph lint tests/06-fib.glyph

# Start the IDE (requires X11)
./glyphide tests/17-fizzbuzz.glyph

# Show help
./glyph --help
```

## Building from Source

```bash
make
make test
```

## Requirements

- Linux kernel 3.2+
- glibc 2.17+
- For IDE: X11 (libx11-dev on Debian/Ubuntu)
