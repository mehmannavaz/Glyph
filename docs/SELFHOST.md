# Self-Hosting — Glyph written in Glyph

> *The ultimate test of a language: can it host itself?*
> *Glyph v0.2 takes the first step.*

## What is Self-Hosting?

A self-hosting compiler is one where the compiler is written in the language
it compiles. Examples: C (gcc is written in C), Go (go is written in Go),
Rust (rustc is written in Rust).

## Current Status: v0.1 (Bootstrap)

`selfhost/glyphc.glyph` is a **minimal Glyph interpreter written in Glyph**.
It proves that Glyph is expressive enough to host itself.

### What Works
- **Lexer**: tokenizes Glyph source code into an array of tokens
- **Integer literals**: `42`, `0`, `12345`
- **String literals**: `"hello"`, `"world"`
- **Addition**: `1 + 2`, `"hello " + "world"`
- **Subtraction**: `100 - 37`
- **Print**: `print expr`
- **REPL**: interactive prompt with `:q` to quit

### What's Planned (v0.2+)
- Variable assignment and lookup
- Squire definitions and calls
- Loops (`(loop)`, `(N)`, `(for i in a..b)`)
- Guards (`<if cond>`)
- Triggers (`{name}`)
- Full self-hosting: the C compiler becomes a bootstrap only

## Running the Self-Hosting Compiler

```bash
$ glyph selfhost/glyphc.glyph
Glyph Self-Hosting Compiler v0.1
A Glyph interpreter written in Glyph.

Supported: print, integers, strings, +, -
Type :q to quit

glyphc> print 42
42
glyphc> print 1 + 2
3
glyphc> print "hello " + "world"
hello world
glyphc> print 100 - 37
63
glyphc> :q
Goodbye!
```

## How It Works

The self-hosting compiler uses these Glyph features:
- Arrays as token lists: `[[kind, text], ...]`
- String indexing: `src[i]` returns a single character
- Character comparison: `c >= 48 && c <= 57` (works because single-char
  strings are treated as their char code in comparisons)
- String concatenation: `s += ch` builds identifiers
- Recursive squires: `eval_expr` calls itself for nested expressions
- `push()` to build arrays dynamically

## The Bootstrap Path

```
1. C compiler (src/*.c) → glyph binary (current, full-featured)
2. glyph binary → runs selfhost/glyphc.glyph (minimal interpreter)
3. Future: glyphc.glyph becomes full compiler → replaces C code
4. C compiler becomes bootstrap only (like C's relationship to assembly)
```

This is the same path Go, Rust, and many other languages took.

## Known Limitations

The self-hosting compiler cannot yet handle variables because of a memory
issue with array index assignment (`g_vals[i] = val`) in the C interpreter.
This is documented in `docs/PROBLEMS.md` and will be fixed in v0.3.
