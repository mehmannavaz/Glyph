# GLYPH — A Graph+Text Programming Language

> *"Programs are meant to be read by humans and only incidentally for computers to execute." — H. Abelson & G. Sussman, SICP*
>
> GLYPH makes that literal: every block is drawn with a **shape** drawn in punctuation, and **text** flows inside.

## 1. Why

Ken Thompson and Dennis Ritchie taught us: **one tool, one job, composable through pipes.**
GLYPH follows that.

* **One shape = one role.** No more "is `()` a function call, a tuple, or a loop?" — in GLYPH every bracket has exactly one meaning.
* **Text is the truth.** Source is plain ASCII/UTF-8 text. The shapes are **visible** in any text editor — no proprietary binary format, no drag-and-drop IDE required.
* **The graph is the program.** Because every block has a unique visual signature, you can literally *see* the program's structure at a glance, the way a flowchart designer draws it.

## 2. The Four Shapes

| Shape            | Punctuation | Name     | Role                                                    |
|------------------|-------------|----------|---------------------------------------------------------|
| **Square**       | `[name]`    | *SQUIRE* | Define a named block / reusable unit                    |
| **Circle**       | `(name)`    | *LOOP*   | Iteration, finite or infinite                           |
| **Diamond**      | `<name>`    | *GUARD*  | Conditional: branch on truth                            |
| **Hexagon**      | `{name}`    | *TRIGGER*| Event handler (runtime attaches; fires on condition)    |

> These four are enough to express **sequence, selection, iteration, and exception** — Böhm & Jacopini (1966) proved it.

## 3. Lexical Grammar

```
comment       := '#' until-end-of-line
identifier    := [A-Za-z_][A-Za-z0-9_]*
integer       := [0-9]+
float         := [0-9]+'.'[0-9]+
string        := '"' (escaped | non-quote)* '"'
op            := '+' | '-' | '*' | '/' | '%' | '=' | '==' | '!=' | '<' | '>'
              | '<=' | '>=' | '&&' | '||' | '!' | '&' | '|' | '^' | '<<' | '>>'
square_open   := '['      square_close := ']'
paren_open    := '('      paren_close  := ')'
brace_open    := '{'      brace_close  := '}'
angle_open    := '<'      angle_close  := '>'
newline       := '\n'     semicolon    := ';'
```

Whitespace is **insignificant** except inside strings. A `newline` *or* `;` ends a statement.

The shape punctuation is *only* a block delimiter when it appears as the **first non-whitespace token** of a statement. Inside an expression, `()`, `[]`, `<>` may appear normally as part of sub-expressions (function calls, indexing, comparison).

> This means `(x)` is a *loop named x*, but `f(x)` is a *function call*. The parser distinguishes them by context: at statement start, the shape opens a block.

## 4. Block Syntax

### 4.1 Square `[name]` — SQUIRE (definition)

A named block. May take parameters, may produce a value, may be referenced by name later.

```
[squire]
  x = 1
  x = x + 1
  return x
```

With parameters:

```
[add a b]
  return a + b
```

A squire is **executed** by saying `the [squire]` (or just `[squire]` in expression position).

### 4.2 Circle `(name)` — LOOP

A loop. The body repeats. Three forms:

```
(count)            # count times — `count` is an integer literal or variable
(loop)             # infinite — break with `stop` or a guard
(for i in 0..10)   # range — `i` takes each value
```

Inside a loop body, the magic word `iter` yields the current iteration count (1-based).

### 4.3 Diamond `<name>` — GUARD (conditional)

```
<if>
  condition
  then-branch
else
  else-branch
```

Or in the shorter form:

```
<when x > 0>
  do_something
```

### 4.4 Hexagon `{name}` — TRIGGER (event)

A runtime-attached handler. Fires when its condition becomes true. Built-in triggers:

| Hexagon id        | Fires when                                                    |
|-------------------|---------------------------------------------------------------|
| `{div_by_zero}`   | A `/` or `%` operation would divide by zero                   |
| `{overflow}`      | An integer operation overflows                                |
| `{underflow}`     | A float operation underflows toward zero                      |
| `{bound}`         | An array index is out of bounds                               |
| `{signal}`        | The process receives a Unix signal (arg: signal number)       |
| `{custom_name}`   | User-defined; fires when program calls `raise custom_name`    |

```
{div_by_zero}
  print "Cannot divide by zero, recovering with 0"
  return 0
```

A trigger's `return` value becomes the result of the faulting operation; the program continues.

### 4.5 Shape Composition

Shapes nest naturally:

```
[outer]
  (3)
    <if iter == 2>
      print "second iteration"
    else
      print "other"
```

A shape's body is **indented by 2 spaces** (or by 1 tab). The parser tracks indent level via a stack; closing a shape pops one level.

> Indentation is **structural** (like Python) but the shape brackets are **explicit** (like Lisp). You get both safety and visual clarity.

## 5. Statements

```
statement := shape_block
           | assignment
           | expr_statement
           | 'return' expr?
           | 'stop'                 # break out of nearest loop
           | 'next'                 # continue to next iteration
           | 'raise' identifier     # fire a user trigger
           | 'print' expr (',' expr)*
           | 'the' '[' identifier ']'   # invoke a squire by name
```

### 5.1 Assignment

```
name = expr
name := expr         # same thing, alt form
```

Variables are dynamically scoped inside squire bodies; the top level is the global scope.

## 6. Expressions (precedence low → high)

```
expr        := or_expr
or_expr     := and_expr ('||' and_expr)*
and_expr    := not_expr ('&&' not_expr)*
not_expr    := '!' not_expr | cmp_expr
cmp_expr    := add_expr (('=='|'!='|'<'|'>'|'<='|'>=') add_expr)*
add_expr    := mul_expr (('+'|'-') mul_expr)*
mul_expr    := pow_expr (('*'|'/'|'%') pow_expr)*
pow_expr    := unary ('^' unary)*
unary       := '-' unary | '+' unary | primary
primary     := integer | float | string | 'true' | 'false' | 'nil'
             | identifier
             | identifier '(' args? ')'         # function call
             | identifier '[' expr ']'          # index
             | '(' expr ')'                     # grouping (NOT a loop here)
             | '[' identifier ']'               # squire invocation
```

Note: `()` as a **block** only happens at statement start. Inside an expression, `()` is a grouping paren. The lexer/parser disambiguates by position.

## 7. Built-ins

```
print(args...)              → nil     # write to stdout, newline-terminated
write(args...)              → nil     # write to stdout, no newline
readln()                    → string  # read one line from stdin
readint()                   → int
len(x)                      → int     # length of array or string
push(arr, val)              → array   # append, returns arr
range(start, end, step?)    → array
type(x)                     → string  # "int", "float", "string", "array", "nil"
int(x)                      → int
float(x)                    → float
string(x)                   → string
```

## 8. Runtime Properties

* **Numeric tower**: int64 → float64 (auto-promote on overflow or mixed arithmetic).
* **Strings**: immutable byte sequences (UTF-8 transparent).
* **Arrays**: heterogeneous, growable.
* **Truthiness**: `0`, `""`, `nil`, `[]` are false; everything else is true.
* **Errors**: division by zero, index-out-of-bounds — if no hexagon trigger is attached, the program halts with exit code 1 and a diagnostic on stderr.

## 9. Unix Interface

```
glyph                 # REPL / read program from stdin, interpret
glyph file.glyph      # interpret file
glyph -emit-llvm f.g  # emit LLVM IR text to stdout
glyph -jit f.g        # JIT-compile via libLLVM, then run
glyph -c f.g          # compile to native (via llc+gcc if available)
glyph --check f.g     # parse-only, exit 0 if ok
```

Stdin/stdout are sacred. Diagnostics go to stderr. Exit codes follow `sysexits.h`.

## 10. Hello, Squire

```
# hello.glyph — first program
[hello name]
  print "Hello, " + name + "!"

{div_by_zero}
  print "Recovered from div-by-zero"
  return 0

(main)
  the [hello "World"]
  x = 10 / 0         # fires {div_by_zero}, x becomes 0
  print "x =", x
  (3)
    print "iter", iter
```

Run: `glyph hello.glyph` →
```
Hello, World!
Recovered from div-by-zero
x = 0
iter 1
iter 2
iter 3
```

---

*End of language specification. Implementation follows.*
