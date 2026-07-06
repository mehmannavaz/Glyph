# Glyph IDE — A Graphical+Text Development Environment

> *The IDE matches the language: shapes are colored, text is editable, the graph is visible.*

## Overview

The Glyph IDE (`glyphide`) is a lightweight X11-based code editor designed specifically for the Glyph programming language. It follows the Unix philosophy: one tool, one job, composable through pipes.

## Features

### Code Editor
- Full text editing with cursor movement, selection, copy/paste
- Line numbers
- Auto-indentation (2 spaces, extra indent inside shape blocks)
- Syntax-aware coloring: each shape type gets a distinct color

### Shape-Aware Syntax Highlighting
The IDE colors each block type differently, making the program's structure visible at a glance:

| Shape | Color | Example |
|-------|-------|---------|
| `[squire]` | Yellow | Function definitions |
| `(loop)` | Green | Iteration blocks |
| `<guard>` | Orange | Conditionals |
| `{trigger}` | Red | Event handlers |
| `# comment` | Gray | Comments |

### Shape Preview Panel
A visual panel on the right side shows the block structure as colored boxes, indented to show nesting. You can literally SEE the program's flowchart.

### Run Button
Press `Ctrl+R` or click "Run" to execute the current code with the `glyph` interpreter. Output appears in the right panel.

### Lint Integration
Click "Lint" to run `glyph lint` on the current code. Warnings and errors appear in the output panel.

### File Operations
- `Ctrl+S` — Save
- `Ctrl+O` — Open
- `Ctrl+Q` — Quit

## Usage

```bash
# Start the IDE with a file
glyphide myprogram.glyph

# Start with a new file
glyphide
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Ctrl+R | Run the current code |
| Ctrl+S | Save the file |
| Ctrl+O | Open a file |
| Ctrl+Q | Quit the IDE |
| Esc | Close output panel |
| Arrow keys | Move cursor |
| Page Up/Down | Scroll by page |
| Home/End | Move to start/end of line |
| Enter | Insert newline (with auto-indent) |
| Backspace/Delete | Delete character |

## Architecture

The IDE is a single C file (`src/glyphide.c`) that links directly against libX11. No external dependencies, no framework, no bloat.

```
src/glyphide.c    — the entire IDE (~600 lines of C)
src/screenshot.c  — screenshot utility for testing
```

## Building

```bash
make            # builds both glyph and glyphide
make ide        # builds just glyphide
```

## The Linter (`glyph lint`)

The linter performs static analysis on Glyph source code:

```bash
glyph lint myprogram.glyph
```

Checks:
- Syntax errors (parse failures)
- Missing `(main)` block
- Empty squire/trigger bodies
- Undefined squire invocations (planned)
- Assignment in condition (planned)

Exit code 0 = no issues, non-zero = errors found.

## Philosophy

The IDE follows the same principles as the language:

1. **Text is the truth.** The editor is a text editor — no proprietary format, no binary files.
2. **The graph is visible.** Shape coloring and the preview panel make the block structure immediately apparent.
3. **One tool, one job.** The IDE edits and runs code. It doesn't try to be a debugger, profiler, or package manager. Use separate Unix tools for those.
4. **Composable.** The IDE calls `glyph` to run code, `glyph lint` to check it. You can use `glyph` independently in scripts, pipes, and CI.

## Screenshot

The IDE has been tested with Xvfb (virtual framebuffer). Screenshots are saved as PPM files in the `download/` directory.

```
glyphide_01.ppm    — IDE with hello.glyph loaded
glyphide_02.ppm    — IDE with fizzbuzz.glyph loaded
visual_demo.ppm    — The visual.glyph demo running (real X11 window)
```
