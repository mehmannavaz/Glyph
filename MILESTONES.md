# Glyph Milestones

A roadmap for the Glyph programming language, in the spirit of Ken
Thompson and Dennis Ritchie's Unix philosophy: one tool, one job,
composable through pipes.

---

## v0.1.0 — Initial Release ✅

The core language: four block shapes (squire, loop, guard, trigger),
a tree-walking interpreter, an experimental LLVM JIT, and an X11 IDE.

- Lexer, parser, AST, tree-walking interpreter
- 18 tests, all pass
- X11 graphical IDE (`glyphide`)
- `man glyph` page

## v0.2.0 — FFI Foundation ✅

Call any language from Glyph. Every language is a filter, spoken to
via a tiny line-delimited JSON protocol over subprocess pipes.

- `dlopen`, `dlsym`, `ccall` for C shared libraries
- `ccallf` for typed FFI calls with proper float/double passing
- Pointer operations: `ptr_read`, `ptr_write`, `malloc`, `free`
- Language-agnostic FFI: `exec`, `pipe_open`/`pipe_write`/`pipe_readln`/`pipe_close`
- `lang_eval`, `lang_call`, `lang_list` for any language
- Inline `[langname]` block syntax — write Python/Node/Bash/C/Rust/Zig/Nim/Elm directly inside Glyph
- Per-language adapter scripts in `lib/lang/*.sh`
- 25 tests

## v0.3.0 — The Plan 9 Release (CURRENT) 🚧

**Theme: "Everything is a file. Everything is a filter. Everything composes."**

Glyph becomes a real Unix-style scripting language. You can write
`cat`, `wc`, `grep`, `ls`, `head`, `tail`, `sort`, `uniq` in Glyph
itself, and they compose with real Unix pipes.

### String operations
- `str_find`, `str_find_from` — substring search
- `str_slice` — substring by index range
- `str_split`, `str_join` — split/join on delimiter
- `str_replace`, `str_replace_all` — substitution
- `str_trim`, `str_trim_left`, `str_trim_right` — whitespace trimming
- `str_upper`, `str_lower` — case conversion
- `str_starts_with`, `str_ends_with`, `str_contains` — predicates
- `str_repeat`, `str_reverse` — utilities
- `str_chars`, `str_bytes`, `str_from_bytes` — character/byte conversion
- `str_to_int`, `str_to_float`, `int_to_str`, `float_to_str` — numeric conversion (with base support)
- `str_format` — printf-style formatting

### Data structures
- **Dict** (hash table with FNV-1a + linear probing): `dict`, `dict_set`, `dict_get`, `dict_get_or`, `dict_has`, `dict_del`, `dict_keys`, `dict_vals`, `dict_size`, `dict_clear`
- **Array**: `arr_push`, `arr_pop`, `arr_shift`, `arr_unshift`, `arr_map`, `arr_filter`, `arr_reduce`, `arr_sort`, `arr_reverse`, `arr_concat`, `arr_slice`, `arr_find`, `arr_contains`

### File I/O
- `file_open`, `file_close`, `file_read`, `file_readln`, `file_read_all`, `file_write`, `file_writeln`, `file_eof`, `file_seek`, `file_tell`, `file_flush`
- `file_size`, `file_exists`, `file_is_dir`, `file_stat`
- `file_mkdir`, `file_rmdir`, `file_unlink`, `file_rename`, `file_list`
- `file_read_file`, `file_write_file` (convenience)

### JSON
- `json_parse` — parse JSON string to Glyph value
- `json_stringify` — serialize Glyph value to JSON
- `json_stringify_pretty` — with indentation

### Math
- Trig: `math_sin`, `math_cos`, `math_tan`, `math_asin`, `math_acos`, `math_atan`, `math_atan2`
- Log/exp: `math_log`, `math_log2`, `math_log10`, `math_exp`
- Rounding: `math_floor`, `math_ceil`, `math_round`
- Comparison: `math_min`, `math_max`, `math_abs`, `math_sign`, `math_clamp`
- Random: `math_random`, `math_random_int`, `math_random_seed`
- Constants: `math_pi`, `math_e`

### Time
- `time_now`, `time_now_s`, `time_now_ns`
- `time_sleep`, `time_sleep_ms`, `time_sleep_ns`
- `time_format`, `time_parse`
- `time_year`, `time_month`, `time_day`, `time_hour`, `time_min`, `time_sec`, `time_weekday`

### Process
- `proc_getpid`, `proc_getppid`
- `proc_env`, `proc_env_set`, `proc_env_unset`, `proc_env_list`
- `proc_cwd`, `proc_chdir`
- `proc_fork`, `proc_wait`, `proc_wait_any`, `proc_kill`

### Functional
- `call(name, args)` — call a squire by name from C
- `apply(name, args)` — alias
- `arr_map(arr, name)`, `arr_filter(arr, name)`, `arr_reduce(arr, name, init)`, `arr_sort(arr, name?)`

### Plan 9 examples (written in Glyph)
- `cat.glyph` — concatenate files (like `cat`)
- `wc.glyph` — word/line/char count (like `wc`)
- `grep.glyph` — text search (like `grep`)
- `ls.glyph` — directory list (like `ls`)
- `head.glyph` — first N lines (like `head`)
- `tail.glyph` — last N lines (like `tail`)
- `sort.glyph` — sort lines (like `sort`)
- `uniq.glyph` — unique adjacent lines (like `uniq`)
- `tee.glyph` — copy stdin to file and stdout (like `tee`)
- `echo.glyph` — echo args (like `echo`)

### Documentation
- Updated README with full stdlib reference
- `docs/stdlib.md` — complete builtin reference
- `docs/tutorial.md` — getting started guide
- Man page update

### Tests
- 26-string, 27-dict, 28-files, 29-json, 30-math, 31-functional, 32-time, 33-proc

## v0.4.0 — The Self-Hosting Release (FUTURE)

Glyph writes itself.

- Glyph-to-C translator written in Glyph
- Subset of the interpreter rewritten in Glyph
- Package manager (`glyph pkg install`)
- Module system (`import "foo"`)
- Standard library as Glyph source (not C)

## v0.5.0 — The Concurrent Release (FUTURE)

Go-style concurrency, Plan 9 style.

- Coroutines (`(async) ... (await ...)`)
- Channels (`chan`, `<-`, `->`)
- `select` over multiple channels
- Networking (`net_listen`, `net_dial`, `net_accept`)
- HTTP client/server

## v0.6.0 — The Performance Release (FUTURE)

- Bytecode VM (replace tree-walking interpreter)
- Generational GC
- Type inference (optional annotations for speed)
- SIMD primitives
- AOT compilation to native binaries

---

## Versioning

Glyph follows semantic versioning. The language is pre-1.0, so:
- Minor version bumps (0.x.0) add features
- Patch version bumps (0.x.y) fix bugs
- 1.0.0 will be the first "stable" release when the language and
  stdlib are frozen

## Release Schedule

Releases happen when ready, not on a fixed schedule. The Unix way:
"ship when it's done, and it's done when it's right."
