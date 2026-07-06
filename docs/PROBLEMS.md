# Glyph — Problems & Solutions

> *A language that knows its own flaws and documents them is more trustworthy
> than one that pretends to be perfect.* — The Unix way.

This document lists every known problem in Glyph v0.2.0, the root cause, and
the solution (implemented or planned). It is the honest record of what works,
what doesn't, and what to do about it.

---

## Problem 1: Verbose string concatenation

**Status:** SOLVED in v0.2.0

### The Problem
Before v0.2.0, building strings with embedded values was painful:
```glyph
print "x = " + string(x) + ", y = " + string(y)
```
Every value needed explicit `string()` conversion.

### The Solution: String Interpolation
```glyph
print "x = #{x}, y = #{y}"
```
The parser detects `#{expr}` inside string literals, extracts the expression,
re-lexes and parses it, wraps it in `string()`, and builds a concatenation
chain. This is a compile-time transformation — zero runtime cost.

**Files changed:** `src/parse.c` — `parse_primary()` T_STRING case.

### Bonus: Auto-conversion in `+`
The `+` operator already auto-converts when either side is a string:
```glyph
print "count = " + 42      # works! no string() needed
print "price = $" + 9.99   # works!
```

---

## Problem 2: `print` is too verbose for quick output

**Status:** SOLVED in v0.2.0

### The Problem
```glyph
print "hello"
print x
print "result:", x + y
```
Writing `print` everywhere is tedious, especially in a REPL or quick scripts.

### The Solution: `?` shorthand
```glyph
? "hello"
? x
? "result:", x + y
```
`?` is a single-character print operator. The lexer emits `T_QUESTION`, and
`parse_stmt` treats it as a `print` statement.

**Files changed:** `src/glyph.h` (token), `src/lex.c` (lexer), `src/parse.c` (parser).

---

## Problem 3: No interactive coding environment

**Status:** SOLVED in v0.2.0

### The Problem
Testing small snippets required writing a file, running it, editing, re-running.
This is slow and kills experimentation.

### The Solution: REPL mode
```bash
$ glyph repl
Glyph REPL v0.2.0 — type :q to quit, :h for help
glyph> x = 42
glyph> ? "x = #{x}"
x = 42
glyph> ? x * 2
84
glyph> :q
```

**Commands:**
- `:q` / `:quit` — exit REPL
- `:h` / `:help` — show help
- `:c` / `:clear` — clear all variables

Variables persist between lines. Each line is parsed and executed at top level.
Errors are reported but don't crash the REPL.

**Files changed:** `src/main.c` — `MODE_REPL` case, `src/interp.c` — `interp_reset_env()`.

---

## Problem 4: `<` and `>` ambiguity between guards and comparison

**Status:** SOLVED in v0.1.0

### The Problem
Glyph uses `<if cond>` for guards and `<` / `>` for comparison operators.
When a guard condition contains `<` or `>`, the parser must distinguish:
```glyph
<if x > 2>        # Is the first > a guard-close or comparison?
```

### The Solution: Contextual lexing + parser lookahead
1. The lexer emits `T_ANGLE_O` for `<` at statement start, `T_LT` otherwise.
2. `>` is always `T_GT`.
3. The parser tracks `angle_depth` — inside a guard header, the first `>` at
   the top level (not inside parens) closes the guard.
4. **Lookahead:** if the token after `>` can start an expression (number,
   identifier, string, `(`), then `>` is a comparison operator. Otherwise it's
   the guard close.

**Files changed:** `src/lex.c`, `src/parse.c` — `parse_cmp()`.

---

## Problem 5: `import` + loop interaction (memory issue)

**Status:** WORKAROUND in v0.2.0, ROOT CAUSE under investigation

### The Problem
When a program uses `import "x11.glyph"` AND has a `(loop)` block that calls
imported squires, the interpreter sometimes reports "no squire named 'x11_open'"
despite the squire being registered.

### The Workaround
Inline the library code directly in the main file instead of using `import`.
The `examples/visual.glyph` file uses this approach and works correctly.

### Root Cause (suspected)
The import preprocessor expands the source by splicing file contents. This
shifts line numbers. When a large expanded file is parsed and a `(loop)` block
calls a squire defined earlier in the expanded source, there may be a memory
corruption issue in the environment lookup. Direct testing shows:
- Squires ARE registered (verified via `type(x11_open)` returning "squire")
- But `the [x11_open ...]` inside a loop fails to find them
- The same code WITHOUT a loop works perfectly

### Planned Fix
Replace the text-based import preprocessor with a proper module system that
loads and parses each file separately, then merges ASTs. This avoids the
line-number and memory issues of text splicing.

---

## Problem 6: No floating-point support in LLVM IR generation

**Status:** KNOWN LIMITATION, v0.2.0

### The Problem
The LLVM IR emitter (`src/irgen.c`) only handles integer values. Floats,
strings, and arrays are not supported in `--emit-llvm` or `--jit` mode.

### The Solution
Use the interpreter (`glyph file.glyph`) for all programs. The IR generator is
experimental and only useful for simple integer programs.

### Planned Fix
Add float and string types to the IR generator. This requires:
- Type inference to determine if a value is int or float
- LLVM `double` type for floats
- String global constants with proper GEP instructions

---

## Problem 7: No map/dictionary type

**Status:** PLANNED for v0.3.0

### The Problem
Glyph has arrays but no key-value maps. Developers must use parallel arrays or
encode keys as strings, which is error-prone.

### Planned Solution
Add a map literal syntax:
```glyph
m = {"name": "Alice", "age": 30}
? m["name"]
m["age"] = 31
```
This reuses the `{` `}` brackets (currently only used for triggers at statement
start). Inside an expression, `{...}` would be a map literal.

---

## Problem 8: No error recovery in the parser

**Status:** KNOWN LIMITATION

### The Problem
When the parser encounters a syntax error, it stops immediately. It doesn't
try to recover and report multiple errors.

### Planned Solution
Add error synchronization points: after a newline or semicolon, the parser
can attempt to continue. This would allow reporting multiple errors per run,
which is especially useful in the IDE.

---

## Problem 9: Arrays are not garbage collected

**Status:** KNOWN LIMITATION

### The Problem
Array storage is heap-allocated but never freed during execution. For long-
running programs, this leaks memory.

### Current Behavior
- Arrays are allocated with `malloc` and never freed
- The OS reclaims all memory when the program exits
- For short scripts (the common case), this is fine
- For long-running servers, this is a problem

### Planned Solution
Implement reference counting for array storage. The `value` struct already has
a `refcount` field reserved for this purpose.

---

## Problem 10: No multi-line strings

**Status:** PLANNED for v0.3.0

### The Problem
There's no way to write a string spanning multiple lines without `\n` escapes:
```glyph
print "Line 1\nLine 2\nLine 3"
```

### Planned Solution
Add triple-quoted strings:
```glyph
print """
Line 1
Line 2
Line 3
"""
```

---

## Problem 11: No struct/record type

**Status:** PLANNED for v0.3.0

### The Problem
Complex data must be represented as arrays with positional indices:
```glyph
person = ["Alice", 30, "engineer"]
? person[0]  # name
? person[1]  # age
```
This is fragile and unreadable.

### Planned Solution
Add named field access on arrays (syntactic sugar):
```glyph
person = [name: "Alice", age: 30, role: "engineer"]
? person.name
? person.age
```
This avoids adding a new type — arrays gain optional field labels.

---

## Problem 12: Limited FFI (no struct passing, no float args)

**Status:** KNOWN LIMITATION

### The Problem
The FFI (`ccall`) supports up to 12 integer/pointer arguments. It does NOT
support:
- Passing structs by value
- Float/double arguments (they're truncated to int)
- More than 12 arguments

### Current Workaround
For X11 (which uses int and pointer args), this is sufficient. For libraries
that require float args or struct passing, write a small C wrapper.

### Planned Solution
Integrate `libffi` properly (it's installed but we don't use the header).
This would handle all calling conventions correctly.

---

## Problem 13: No package manager

**Status:** PLANNED for v0.4.0

### The Problem
There's no way to install third-party Glyph libraries. The `import` system
only handles local files.

### Planned Solution
A simple package manager:
```bash
glyph install x11        # downloads to ~/.glyph/pkg/x11/
glyph install sqlite     # downloads to ~/.glyph/pkg/sqlite/
```
Imports would search `~/.glyph/pkg/` automatically.

---

## Problem 14: IDE lacks split editor + preview

**Status:** PLANNED for v0.3.0

### The Problem
The IDE shows code on the left and shape preview on the right, but there's no
live "run output" split view. You must press Run to see output.

### Planned Solution
Add a three-panel layout:
- Left: code editor
- Center: live shape preview (updates as you type)
- Right: output panel (shows results automatically)

---

## Summary

| Problem | Status | Solution |
|---------|--------|----------|
| Verbose string concat | SOLVED | `#{expr}` interpolation |
| Verbose `print` | SOLVED | `?` shorthand |
| No REPL | SOLVED | `glyph repl` |
| `<` `>` ambiguity | SOLVED | Contextual lexing + lookahead |
| Import + loop bug | WORKAROUND | Inline library code |
| No float in IR | KNOWN | Use interpreter |
| No maps | PLANNED | `{key: value}` syntax |
| No error recovery | KNOWN | Sync points |
| No GC | KNOWN | Refcounting (field reserved) |
| No multi-line strings | PLANNED | `"""..."""` |
| No structs | PLANNED | Named array fields |
| Limited FFI | KNOWN | Integrate libffi |
| No package manager | PLANNED | `glyph install` |
| IDE layout | PLANNED | Three-panel split |

---

*This document is updated every release. If you find a problem not listed here,
report it. Honesty about limitations is the Unix way.*
