# Glyph Windows Release

## Quick Start (Windows)

1. Open Command Prompt or PowerShell
2. Navigate to this folder
3. Run:
```
glyph.exe tests\01-hello.glyph
```

Output:
```
Hello, World!
```

## What's Included

- `glyph.exe` — the Glyph interpreter, compiler, linter, and REPL (x86-64, 520 KB)
- No external dependencies — pure static Windows binary

## Usage

```cmd
:: Run a program
glyph.exe tests\01-hello.glyph

:: Start the REPL
glyph.exe repl

:: Lint a file
glyph.exe lint tests\06-fib.glyph

:: Check syntax
glyph.exe -c tests\17-fizzbuzz.glyph

:: Show help
glyph.exe --help
```

## Building from Source (Windows)

If you have MinGW-w64 installed:

```cmd
build.bat
```

Or with Make (if available):
```cmd
make EXE=.exe
```

## Cross-Compiling from Linux

```bash
make CC=x86_64-w64-mingw32-gcc EXE=.exe
```

## Notes

- This is a 64-bit Windows executable (x86-64)
- Requires Windows 7 or later
- The IDE (`glyphide`) is X11-only and not included in the Windows build
- FFI uses `LoadLibrary`/`GetProcAddress` instead of `dlopen`/`dlsym`
- All 24 tests pass on Windows when run with MinGW
