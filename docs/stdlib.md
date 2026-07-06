# Glyph Standard Library Reference

All built-in functions are available in every scope. They are implemented as C `native_fn` pointers in `src/interp.c`.

## I/O

### `print(args...) -> nil`
Write each argument to stdout, separated by spaces, terminated by a newline. Flushes stdout.

```glyph
print "x =", 42, "y =", 3.14
```

### `write(args...) -> nil`
Write each argument to stdout with no separator and no newline. Flushes stdout. Useful for prompts before `readln()`.

```glyph
write "Name: "
name = readln()
```

### `readln() -> string`
Read one line from stdin (up to and including the newline, which is discarded). Returns empty string at EOF.

### `readint() -> int | nil`
Read a whitespace-delimited integer from stdin. Returns `nil` if no integer could be parsed.

## Collections

### `len(x) -> int`
Return the length of a string or array.

```glyph
len("hello")    # 5
len([1,2,3])    # 3
```

### `range(start, end, step?) -> array`
Generate an array of integers from `start` (inclusive) to `end` (exclusive), stepping by `step` (default 1). Step may be negative.

```glyph
range(0, 5)        # [0, 1, 2, 3, 4]
range(0, 10, 2)    # [0, 2, 4, 6, 8]
range(10, 0, -1)   # [10, 9, 8, 7, 6, 5, 4, 3, 2, 1]
```

### `push(arr, val) -> array`
Append `val` to `arr` (mutates in place). Returns `arr` for chaining.

```glyph
a = range(0, 3)
push(a, 99)
print a    # [0, 1, 2, 99]
```

## Type Conversion

### `type(x) -> string`
Return the type name: `"int"`, `"float"`, `"string"`, `"array"`, `"nil"`, `"squire"`, `"native"`.

### `int(x) -> int`
Convert to integer. Strings are parsed with `strtoll`. Floats are truncated.

### `float(x) -> float`
Convert to float. Strings are parsed with `strtod`.

### `string(x) -> string`
Convert to string representation. Integers print in decimal; floats print with a trailing `.0` if whole; arrays print as `[a, b, c]` with strings quoted.

## Math

### `pow(a, b) -> int | float`
Power. Returns int if both args are int and the result fits in int64; otherwise float.

### `sqrt(x) -> float`
Square root.

### `abs(x) -> int | float`
Absolute value. Preserves the input type.

## System

### `clock() -> int`
Current Unix timestamp in seconds.

### `exit(code) -> nil`
Exit the program immediately with status `code`.

### `argc() -> int`
Number of program arguments, including `argv[0]` (the program file name).

### `argv() -> array`
Array of program argument strings. `argv()[0]` is the program file name.

### `system(cmd) -> int`
Run a shell command via `system(3)`. Returns the exit status.

## String

### `upper(s) -> string`
Return uppercase copy of string `s`.

### `lower(s) -> string`
Return lowercase copy of string `s`.

## Indexing

Strings and arrays support 0-based indexing with `[]`. Negative indices count from the end.

```glyph
s = "hello"
s[0]         # "h"
s[-1]        # "o"

a = [10, 20, 30]
a[1]         # 20
```

Out-of-bounds access fires the `{bound}` trigger if registered; otherwise it's a runtime error.

## Operators

### Arithmetic
`+ - * / % ^`

`+` on strings concatenates. `^` is power (integer if both operands are non-negative ints and the result fits, otherwise float). `/` and `%` check for zero and fire `{div_by_zero}`.

### Comparison
`== != < <= > >=`

Compares numerically for numbers, lexicographically for strings.

### Logical
`&& || !`

Short-circuit. Truthiness: `0`, `""`, `nil`, `[]` are false; everything else is true.

### Bitwise
`& | << >>`

Integer-only. Bitwise XOR is not directly supported in v1 (use `(a & ~b) | (~a & b)`).
