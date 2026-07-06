# Building Glyph — Cross-Platform Guide

Glyph is designed to build and run on **any operating system** with a C11 compiler.
No external dependencies required for the core interpreter.

## Quick Start

### Linux
```bash
make            # builds glyph + glyphide
make test       # run 24-test suite
./glyph tests/01-hello.glyph
```

### macOS
```bash
# Install X11 (optional, for the IDE):
brew install libx11

make            # builds glyph + glyphide
make test
./glyph tests/01-hello.glyph
```

### Windows (MinGW)
```cmd
build.bat       # builds glyph.exe
glyph.exe tests\01-hello.glyph
glyph.exe repl
```

Or with Make:
```cmd
make EXE=.exe
```

## Platform Support Matrix

| Feature | Linux | macOS | Windows |
|---------|-------|-------|---------|
| Core interpreter | YES | YES | YES |
| All 24 tests | YES | YES | YES |
| REPL (`glyph repl`) | YES | YES | YES |
| Linter (`glyph lint`) | YES | YES | YES |
| String interpolation | YES | YES | YES |
| `?` print shorthand | YES | YES | YES |
| FFI (`dlopen`/`ccall`) | YES | YES | YES* |
| IDE (`glyphide`) | YES | YES | NO** |
| X11 GUI programs | YES | YES | NO** |
| LLVM JIT | YES | Planned | Planned |

*Windows uses `LoadLibrary`/`GetProcAddress` (transparent via `platform.h`)
**Windows uses Win32 API, not X11. A Win32 IDE is planned.

## Architecture

The cross-platform support is built on a single principle: **one abstraction layer**.

```
src/platform.h    ← The ONLY file with #ifdef _WIN32
src/glyph.h       ← Language types (platform-independent)
src/*.c           ← All include platform.h, never platform-specific headers
```

Every source file includes `platform.h` instead of `<unistd.h>`, `<dlfcn.h>`, etc.
The platform header maps:

| Unix | Windows |
|------|---------|
| `dlopen` | `LoadLibraryA` |
| `dlsym` | `GetProcAddress` |
| `dlclose` | `FreeLibrary` |
| `sysexits.h` | inline defines |
| `unistd.h` | `<io.h>` + stubs |
| `signal.h` | stubbed (Windows uses different signal model) |
| `/proc/self/exe` | `GetModuleFileName` |
| `popen`/`pclose` | `_popen`/`_pclose` |
| `strtok_r` | `strtok` |

## Build Options

### Makefile variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CC` | `gcc` | C compiler |
| `CFLAGS` | `-std=c11 -Wall -O2 -g` | Compile flags |
| `LDFLAGS` | (empty) | Link flags |
| `LDLIBS` | `-ldl -lm` (Linux) | Libraries |

### Cross-compilation

To cross-compile for Windows from Linux:
```bash
make CC=x86_64-w64-mingw32-gcc EXE=.exe LDLIBS="-lm"
```

To cross-compile for 32-bit:
```bash
make CC=i686-w64-mingw32-gcc EXE=.exe LDLIBS="-lm"
```

## Dependencies

### Core interpreter
- **C11 compiler** (gcc, clang, MSVC)
- **Standard C library**
- That's it.

### IDE (glyphide)
- **X11** (Linux, macOS via brew, BSD)
- Not available on Windows (Win32 port planned)

### FFI
- **dlopen** (Unix) or **LoadLibrary** (Windows) — both built in

### LLVM JIT (experimental)
- **libLLVM** (any version) — loaded at runtime via dlopen/LoadLibrary
- No dev headers needed

## Verifying Your Build

```bash
make test
```

All 24 tests should pass on every platform. If a test fails, it's a bug — report it.

## Troubleshooting

### "Cannot find -lX11"
You're on a system without X11 dev headers. Install:
- Ubuntu/Debian: `sudo apt install libx11-dev`
- macOS: `brew install libx11`
- Or build without the IDE: `make glyph`

### "dlfcn.h not found" (Windows)
This shouldn't happen — `platform.h` maps to `LoadLibrary` on Windows.
If you see this, you're not including `platform.h`. All source files must
include it.

### "time() implicit declaration"
Ensure `platform.h` is included — it provides `<time.h>` on all platforms.

### Tests fail on Windows
Windows uses CRLF line endings. The test expected files use LF. Run:
```bash
# In Git Bash or MinGW:
dos2unix tests/*.expected
```

## The Unix Way

Glyph follows the Unix philosophy even in its build system:

1. **One Makefile** — autodetects platform, no cmake, no configure script
2. **One abstraction** — `platform.h` is the only platform-specific file
3. **Zero dependencies** for the core — just a C11 compiler
4. **Composable** — `glyph` reads stdin, writes stdout, exits with codes
