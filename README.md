# GLYPH

> *A graph+text programming language. Every block is a shape. The shape is the syntax.*

```
[squire]        (loop)         <if cond>       {trigger}
  body            body            body            body
```

Glyph is a small, sharp programming language where the **four block shapes** — `[squire]`, `(loop)`, `<guard>`, `{trigger}` — are drawn directly in ASCII punctuation. You can literally *see* the program's control flow at a glance, the way a flowchart designer draws it. No drag-and-drop IDE required; source is plain text.

Built in C following Ken Thompson and Dennis Ritchie's Unix philosophy: **one tool, one job, composable through pipes.**

Now with a **graphical IDE** (`glyphide`) and **X11 GUI bindings** — write real graphical programs in pure Glyph!

---

## Quick Start

```bash
$ cd glyph/
$ make            # build both `glyph` and `glyphide`
$ make test       # run the 18-test suite — all pass
$ ./glyph tests/01-hello.glyph
Hello, World!
```

## Install

```bash
$ sudo make install    # copies glyph to /usr/local/bin, man page to /usr/local/share/man
$ man glyph
```

---

## The Four Shapes

| Shape | Punctuation | Name | Role |
|-------|-------------|------|------|
| **Square** | `[name]` | SQUIRE | Define a reusable named block |
| **Circle** | `(name)` | LOOP | Iteration: `(loop)` infinite, `(N)` count, `(for i in 0..10)` range, `(main)` run-once |
| **Diamond** | `<if cond>` | GUARD | Conditional with optional `else` |
| **Hexagon** | `{name}` | TRIGGER | Event handler — fires on fault or `raise` |

These four are enough to express **sequence, selection, iteration, and exception** — Böhm & Jacopini (1966) proved it.

---

## Hello, World

```glyph
# hello.glyph
[hello name]
  print "Hello, " + name + "!"

(main)
  the [hello "World"]
```

Run it:
```
$ glyph hello.glyph
Hello, World!
```

---

## The User's Original Example — Working

This is the exact program from the language's original specification, now fully functional:

```glyph
[squire]
  x = 1
  x = x + 1
  print "squire ran, x =", x
  return x

{div_by_zero}
  print "!!! HEXAGON TRIGGER: division by zero caught"
  return 0

(main)
  print "=== running squire 3 times via loop ==="
  count = 0
  (loop)
    count = count + 1
    result = the [squire]
    <if count > 2>
      print "stopping loop after", count, "runs"
      stop

  print "=== triggering hexagon on div by zero ==="
  x = 10 / 0
  print "x =", x

  print "=== guard with comparison ==="
  <if count > 2>
    print "loop ran more than 2 times"
  else
    print "loop ran 2 or fewer times"
```

Output:
```
=== running squire 3 times via loop ===
squire ran, x = 2
squire ran, x = 2
squire ran, x = 2
stopping loop after 3 runs
=== triggering hexagon on div by zero ===
!!! HEXAGON TRIGGER: division by zero caught
x = 0
=== guard with comparison ===
loop ran more than 2 times
```

---

## Triggers (Hexagons)

Triggers are Glyph's exception/event system. When a fault condition occurs, the runtime looks up the matching hexagon; if found, its body runs and its `return` value substitutes the faulting operation's result. The program continues.

### Built-in Triggers

```glyph
{div_by_zero}
  print "Cannot divide by zero, recovering with 0"
  return 0

{overflow}
  print "Integer overflow detected"
  return 0

{bound}
  print "Index out of bounds"
  return -1

{signal}
  print "Received signal", signo
```

### User-Defined Triggers

```glyph
{alarm}
  print "*** ALARM FIRED ***"

(main)
  raise alarm
```

---

## Loops

Three loop forms plus the labeled-sequence form:

```glyph
# Counted: runs N times
(5)
  print "iteration", iter

# Range: i takes each value
(for i in 0..10)
  print i

# Range with step
(for i in 0..10, 2)
  print i   # 0, 2, 4, 6, 8

# Infinite: break with `stop`
(loop)
  <if iter > 100>
    stop

# Labeled sequence: runs once
(main)
  print "this is the entry point"
```

Inside any loop, the magic variable `iter` holds the current iteration count (1-based).

---

## Squires (Squares)

A squire is a named, parameterized block. Call it with `the [name args...]`:

```glyph
[fib n]
  <if n < 2>
    return n
  else
    return the [fib n - 1] + the [fib n - 2]

(main)
  (for i in 0..10)
    print "fib(" + string(i) + ") =", the [fib i]
```

Squires support recursion, mutual recursion, and nested definitions.

---

## Guards (Diamonds)

```glyph
[grade score]
  <if score >= 90>
    return "A"
  else
    <if score >= 80>
      return "B"
    else
      return "C"
```

Guards support `else`. The `<` and `>` inside a guard header are disambiguated by the parser: the last `>` on the header line closes the guard; earlier `>` are comparison operators.

---

## Built-in Functions

```
print(args...)              Write args separated by spaces, newline-terminated
write(args...)              Write args without newline (flushes stdout)
readln()                    Read one line from stdin
readint()                   Read an integer from stdin
len(x)                      Length of string or array
range(start, end, step?)    Generate an array of integers
push(arr, val)              Append val to arr, return arr
type(x)                     Return type name as string
int(x)                      Convert to integer
float(x)                    Convert to float
string(x)                   Convert to string
pow(a, b)                   Power
sqrt(x)                     Square root
abs(x)                      Absolute value
clock()                     Unix timestamp in seconds
exit(code)                  Exit with status code
upper(s) / lower(s)         Uppercase / lowercase a string
argc()                      Number of program arguments (including argv[0])
argv()                      Array of program argument strings
system(cmd)                 Run a shell command
```

## Inline Foreign Language Blocks

Glyph can call **any** language inline, using a block syntax that matches
the existing `[squire]` shape. The block runs until the next line dedents
to or below the opener's column — no closer needed.

```glyph
(main)
  print "before"
  [python]
    test = "text"
    print(f"{test}")
  print "after"
```

Output:
```
before
text
after
```

Supported languages (any name in `FOREIGN_LANGS` in `src/lex.c`):

```
python  python3  py
node    nodejs   js  javascript
rust    rs
zig
nim
c       cpp      c++
elm
go      golang
ruby    rb
perl
lua
julia
tcl
bash    sh       shell
awk     sed
```

Each language has a tiny adapter script in `lib/lang/<lang>.sh` that
speaks a line-delimited JSON protocol: Glyph sends `{"op":"eval","code":"..."}`
on stdin, the adapter runs the code and replies `{"value":"..."}` on stdout.

This is the **Plan 9 / Unix way**: every language is a filter. No
special-case binding code per language. Composable, replaceable, hackable.

### How it works

1. The lexer recognises `[langname]` at statement start where `langname`
   is a known foreign language.
2. It captures the raw source text until the next line dedents to or
   below the opener's column (or EOF). The captured text is dedented
   (common leading whitespace stripped) so Python doesn't choke.
3. The parser produces an `A_BLOCK_LANG` node holding the language name
   and the raw body.
4. At runtime, the interpreter calls the `lang_eval` builtin (from
   `src/ffi.c`) which spawns the adapter, sends the code as JSON, and
   reads the response. If the response is a non-empty string, it's
   printed to stdout.

### Adding a new language

1. Add its name to `FOREIGN_LANGS[]` in `src/lex.c`.
2. Write `lib/lang/<lang>.sh` that reads JSON requests on stdin and
   writes JSON responses on stdout. See `lib/lang/python.sh` for a
   template.

That's it. No compiler changes, no runtime changes.

### Low-level FFI primitives

For cases where the inline block syntax isn't enough (programmatic
code generation, persistent subprocesses, calling C shared libraries
directly), Glyph exposes these builtins:

```
exec(cmd, input?) -> string       Run command, capture stdout
exec_status(cmd, input?) -> int   Run command, return exit code
pipe_open(cmd) -> ptr             Spawn persistent subprocess
pipe_write(h, s) -> int           Write to subprocess stdin
pipe_readln(h) -> string          Read one line (nil on EOF)
pipe_read(h, n) -> string         Read up to n bytes
pipe_close(h) -> int              Close subprocess
lang_eval(lang, code) -> string   Eval code in any language
lang_call(lang, mod, fn, args)    Call fn in lang's module
lang_list() -> array              List available languages
dlopen(name) -> ptr               Open shared library
dlsym(handle, name) -> ptr        Look up symbol in library
ccall(handle, name, args, ret)    Call C function (int/ptr args)
ccallf(handle, name, args, ret, argtypes)  Call C function with typed args
ptr_null() -> ptr                 NULL pointer
ptr_read(p, off) -> int           Read 8 bytes at p+off
ptr_write(p, off, val)            Write 8 bytes at p+off
malloc(n) -> ptr                  Allocate n bytes
free(p)                           Free allocation
```

---

## CLI Usage

```
glyph [options] [file] [args...]

Options:
  -i, --interpret   interpret (default)
  --emit-llvm       emit LLVM IR text to stdout
  --jit             JIT-compile via libLLVM-19.so and run
  -c, --check       parse-only, exit 0 on success
  --ast             dump AST (debugging)
  --tokens          dump tokens (debugging)
  -h, --help        show help
  -v, --version     show version
```

If `file` is omitted or `-`, read from stdin. Arguments after `file` are passed to the program as `argv`.

---

## Unix Pipeline Integration

```bash
$ echo '(main) print "hello from pipe"' | glyph
hello from pipe

$ glyph myprog.glyph | wc -l
42

$ cat data.csv | glyph process.glyph | sort | uniq -c
```

Glyph reads stdin, writes stdout, sends diagnostics to stderr. Exit codes follow `sysexits.h`.

---

## Architecture

The compiler is split into Unix-style modules with strict boundaries:

```
src/lex.c     → tokens         (lexer)
src/parse.c   → AST            (recursive descent parser)
src/value.c   → runtime values (int, float, string, array, func)
src/interp.c  → execution      (tree-walking interpreter)
src/irgen.c   → LLVM IR text   (code generator)
src/jit.c     → dlopen(libLLVM) (in-process JIT)
src/main.c    → CLI            (Unix-style driver)
```

Each module owns its state and exposes a small, opinionated API. The interpreter is the primary execution path; the IR generator and JIT are experimental.

### Runtime Properties

- **Numeric tower**: int64 → float64 (auto-promote on overflow or mixed arithmetic)
- **Strings**: immutable byte sequences (UTF-8 transparent)
- **Arrays**: heterogeneous, growable
- **Truthiness**: `0`, `""`, `nil`, `[]` are false; everything else is true
- **Errors**: division by zero, index-out-of-bounds — if no trigger is attached, the program halts with exit code 70

### Control Flow

Control flow signals (`return`, `stop`, `next`, `raise`) are implemented with `longjmp` for clean unwinding. Triggers fire via a runtime hook: arithmetic ops check for fault conditions and consult the trigger table before erroring.

---

## Test Suite

18 tests cover every language feature:

```
01-hello          Hello World
02-squire         Squire definition and invocation
03-loop           Counted, range, infinite, labeled loops
04-hexagon        div_by_zero trigger
05-guard          Nested if/else
06-fib            Recursive Fibonacci
07-arrays          Array operations + bound trigger
08-raise          User-defined trigger
09-strings        String concatenation, indexing, upper/lower
10-recursion      Factorial, GCD, Ackermann
11-overflow       Integer overflow trigger
12-nested         Nested shapes, deep indentation
13-stdio          stdin/stdout
14-expr           All operators and precedence
15-argv           Command-line arguments
16-closures       Nested squires, mutual recursion
17-fizzbuzz       The canonical test
18-spec-example   The user's original spec example, end-to-end
```

Run with `make test`.

---

## Why Glyph?

Ken Thompson and Dennis Ritchie taught us: **one tool, one job, composable through pipes.** Glyph follows that.

- **One shape = one role.** No more "is `()` a function call, a tuple, or a loop?" — in Glyph every bracket has exactly one meaning.
- **Text is the truth.** Source is plain ASCII/UTF-8 text. The shapes are visible in any text editor — no proprietary binary format, no drag-and-drop IDE required.
- **The graph is the program.** Because every block has a unique visual signature, you can literally *see* the program's structure at a glance.

---

## License

Public domain. Do what you want.

---

## Acknowledgments

Designed in the spirit of:
- **Ken Thompson & Dennis Ritchie** — Unix philosophy, C
- **Grace Hopper** — flowcharts as programs
- **Böhm & Jacopini (1966)** — sequence, selection, iteration suffice
- **John McCarthy** — Lisp's homoiconicity (shapes are syntax)

Built with C11, libLLVM-19, and `make`.
