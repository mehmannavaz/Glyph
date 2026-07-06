@echo off
REM build.bat — Windows build script for Glyph
REM
REM Usage:
REM   build.bat           — build glyph.exe
REM   build.bat test      — build and run tests
REM   build.bat clean     — clean build artifacts
REM
REM Requires: MinGW (gcc) installed and in PATH

cd /d "%~dp0"

echo Detecting compiler...
gcc --version >nul 2>&1
if errorlevel 1 (
  echo Error: gcc not found. Install MinGW and add it to PATH.
  exit /b 1
)

echo Building glyph.exe...
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function ^
    -Wno-clobbered -Wno-unused-but-set-variable -Wno-stringop-truncation ^
    -Wno-restrict -O2 -DGLYPH_WINDOWS -D__USE_MINGW_ANSI_STDIO=1 ^
    -o glyph.exe ^
    src\util.c src\lex.c src\ast.c src\parse.c src\value.c ^
    src\interp.c src\irgen.c src\jit.c src\main.c ^
    -lm

if errorlevel 1 (
  echo Build failed!
  exit /b 1
)

echo Build successful: glyph.exe
echo.
echo Quick test:
echo   glyph.exe tests\01-hello.glyph
echo   glyph.exe repl
