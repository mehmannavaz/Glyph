# Glyph Language Tutorial

A 15-minute tour of Glyph, from hello world to recursive data structures.

## 1. Hello, World

```glyph
[hello name]
  print "Hello, " + name + "!"

(main)
  the [hello "World"]
```

Save as `hello.glyph` and run:
```bash
$ glyph hello.glyph
Hello, World!
```

**What's happening?**
- `[hello name]` defines a **squire** (function) named `hello` that takes one parameter `name`.
- `print` writes to stdout with a newline.
- `(main)` is the entry point — a labeled sequence block that runs once.
- `the [hello "World"]` invokes the `hello` squire with argument `"World"`.

## 2. The Four Shapes

Every block in Glyph is one of four shapes:

| Shape | Syntax | Purpose |
|-------|--------|---------|
| Square | `[name params]` | Define a squire (function) |
| Circle | `(loop)` / `(N)` / `(for i in a..b)` / `(main)` | Loop or labeled sequence |
| Diamond | `<if cond>` | Conditional (with `else`) |
| Hexagon | `{event}` | Event handler / trigger |

Bodies are indentation-based: statements indented deeper than the opener belong to the block.

## 3. Variables and Assignment

```glyph
x = 42
name = "Alice"
pi = 3.14159
nums = [1, 2, 3, 4, 5]
nothing = nil
```

Variables are dynamically typed. Assignment uses `=`. The type is determined by the value.

## 4. Squires (Functions)

```glyph
[add a b]
  return a + b

[greet name times]
  (times)
    print "Hello, " + name + "!"

(main)
  print the [add 3 4]          # 7
  the [greet "World" 3]        # prints Hello, World! three times
```

Squires are defined with `[name params...]`. They return with `return`. If no return, they return `nil`.

Call a squire with `the [name args...]`. Arguments are space-separated (commas optional).

## 5. Loops

### Counted loop
```glyph
(5)
  print "iteration", iter
```
`iter` is a magic variable holding the 1-based iteration count.

### Range loop
```glyph
(for i in 0..10)
  print i

(for i in 0..10, 2)
  print i   # 0, 2, 4, 6, 8
```

### Infinite loop
```glyph
(loop)
  <if iter > 100>
    stop
```
Use `stop` to break, `next` to skip to the next iteration.

### Labeled sequence
```glyph
(main)
  print "this runs once"
  print "so does this"
```
Any `(name)` that isn't `loop` runs its body once. `(main)` is the conventional entry point.

## 6. Guards (Conditionals)

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

Guards use `<if condition>`. The condition can use any comparison operator. The `>` that closes the guard header is distinguished from `>` the comparison operator by parser context.

## 7. Triggers (Event Handlers)

Triggers fire when specific runtime events occur:

```glyph
{div_by_zero}
  print "Caught division by zero!"
  return 0

(main)
  x = 10 / 0    # fires {div_by_zero}, x becomes 0
  print x       # 0
```

### Built-in triggers

| Trigger | Fires when |
|---------|-----------|
| `{div_by_zero}` | `/` or `%` by zero |
| `{overflow}` | Integer arithmetic overflows |
| `{bound}` | Array/string index out of bounds |
| `{signal}` | Unix signal received (var `signo`) |

### User-defined triggers

```glyph
{alarm}
  print "*** ALARM ***"

(main)
  raise alarm    # fires the {alarm} trigger
```

A trigger's `return` value substitutes the faulting operation's result. The program continues.

## 8. Arrays

```glyph
nums = [1, 2, 3, 4, 5]
print len(nums)        # 5
print nums[0]          # 1
print nums[-1]         # 5 (negative indexes from end)

nums[0] = 99
print nums             # [99, 2, 3, 4, 5]

push(nums, 6)
print nums             # [99, 2, 3, 4, 5, 6]

# range() generates arrays
evens = range(0, 10, 2)
print evens            # [0, 2, 4, 6, 8]
```

## 9. Strings

```glyph
s = "Hello, " + "World" + "!"
print s                # Hello, World!
print len(s)           # 13
print s[0]             # H
print upper(s)         # HELLO, WORLD!
print lower(s)         # hello, world!
```

Strings are immutable byte sequences. Indexing returns a single-character string. `+` concatenates.

## 10. Recursion

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

Squires can call themselves recursively. Mutual recursion works too:

```glyph
[is_even n]
  <if n == 0>
    return 1
  else
    return the [is_odd n - 1]

[is_odd n]
  <if n == 0>
    return 0
  else
    return the [is_even n - 1]
```

## 11. Putting It Together: FizzBuzz

```glyph
[fizzbuzz n]
  (for i in 1..n + 1)
    <if i % 15 == 0>
      print "FizzBuzz"
    else
      <if i % 3 == 0>
        print "Fizz"
      else
        <if i % 5 == 0>
          print "Buzz"
        else
          print i

(main)
  the [fizzbuzz 15]
```

## 12. Error Recovery

Without a trigger, faults halt the program:

```glyph
(main)
  x = 10 / 0    # runtime error, exit code 70
```

With a trigger, the program continues:

```glyph
{div_by_zero}
  print "Recovering..."
  return 0

(main)
  x = 10 / 0    # prints "Recovering...", x = 0
  print x       # 0
  print "done"  # done
```

## 13. Input and Output

```glyph
(main)
  write("What is your name? ")
  name = readln()
  print "Hello, " + name + "!"

  write("Give me a number: ")
  n = readint()
  print "Doubled:", n * 2
```

`print` adds a newline; `write` doesn't. Both flush stdout.

## 14. Command-Line Arguments

```glyph
(main)
  print "argc =", argc()
  args = argv()
  (for i in 0..len(args))
    print "  argv[" + string(i) + "] =", args[i]
```

Run with: `glyph myprog.glyph arg1 arg2`

## 15. Next Steps

- Read the [Language Specification](language-spec.md) for the formal grammar.
- Read the [Standard Library Reference](stdlib.md) for all built-in functions.
- Browse the `examples/` directory for complete programs:
  - `sort.glyph` — bubble sort + binary search
  - `prime.glyph` — sieve of Eratosthenes
  - `string.glyph` — string processing (reverse, palindrome, word count)
  - `hanoi.glyph` — Tower of Hanoi
  - `linked_list.glyph` — recursive linked list
  - `calc.glyph` — interactive calculator

Welcome to Glyph. May your shapes always be visible.
