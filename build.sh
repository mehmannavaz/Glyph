#!/bin/bash
# build.sh — cross-platform build script for Glyph
#
# Usage:
#   ./build.sh           — build glyph (+ glyphide if X11 available)
#   ./build.sh test      — build and run tests
#   ./build.sh clean     — clean build artifacts
#   ./build.sh install   — install to /usr/local (requires root)
#
# This script works on Linux, macOS, and any Unix-like system.
# For Windows, use build.bat or run: make EXE=.exe

set -e

cd "$(dirname "$0")"

# Detect platform
UNAME_S=$(uname -s 2>/dev/null || echo "Windows")
echo "Platform: $UNAME_S"

case "$UNAME_S" in
  Linux)
    echo "Building for Linux..."
    ;;
  Darwin)
    echo "Building for macOS..."
    # macOS may need to install X11 via brew: brew install libx11
    if ! pkg-config --exists x11 2>/dev/null; then
      echo "Note: X11 not found. Install with: brew install libx11"
      echo "Building without glyphide..."
    fi
    ;;
  MINGW*|MSYS*|CYGWIN*)
    echo "Building for Windows (MinGW)..."
    ;;
  *)
    echo "Unknown platform $UNAME_S, attempting Unix build..."
    ;;
esac

# Run make with the detected platform
case "$1" in
  test)
    make test
    ;;
  clean)
    make clean
    ;;
  install)
    make install
    ;;
  *)
    make
    echo ""
    echo "Build complete."
    echo "  Binary:  ./glyph"
    if [ -f glyphide ]; then
      echo "  IDE:     ./glyphide"
    fi
    echo ""
    echo "Quick test:"
    echo "  ./glyph tests/01-hello.glyph"
    echo "  ./glyph repl"
    ;;
esac
