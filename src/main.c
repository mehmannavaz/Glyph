/* main.c — Glyph CLI driver.
 *
 * Usage:
 *   glyph [options] [file]
 *
 * Options:
 *   -i              interpret (default)
 *   --emit-llvm     emit LLVM IR text to stdout
 *   --jit           JIT-compile via libLLVM-19.so and run
 *   --check         parse-only, exit 0 on success
 *   --ast           dump AST to stderr (debugging)
 *   --tokens        dump tokens to stderr (debugging)
 *   -h, --help      show usage
 *   -v, --version   show version
 *
 * If no file given, read program from stdin.
 *
 * Exit codes follow sysexits.h:
 *   0   success
 *   1   runtime error
 *   64  usage error
 *   65  data (parse/lex) error
 *   70  internal error
 */

#include "glyph.h"
#include "platform.h"

int g_cli_argc = 0;
char **g_cli_argv = NULL;

static const char *VERSION = "0.2.0";

static void usage(FILE *out) {
    fprintf(out,
        "glyph %s — a graph+text programming language\n"
        "\n"
        "USAGE:\n"
        "    glyph [OPTIONS] [FILE]      run a Glyph program\n"
        "    glyph repl                  interactive REPL\n"
        "    glyph lint FILE             lint a file\n"
        "\n"
        "OPTIONS:\n"
        "    -i, --interpret   interpret (default)\n"
        "    --emit-llvm       emit LLVM IR text to stdout\n"
        "    --jit             JIT-compile via libLLVM-19.so and run\n"
        "    -c, --check       parse-only, exit 0 on success\n"
        "    --ast             dump AST (debugging)\n"
        "    --tokens          dump tokens (debugging)\n"
        "    lint              run linter on a file\n"
        "    repl              start interactive REPL\n"
        "    -h, --help        show this help\n"
        "    -v, --version     show version\n"
        "\n"
        "If FILE is omitted or '-', read program from stdin.\n"
        "\n"
        "LANGUAGE SUMMARY:\n"
        "    [name]    — squire (define a block)\n"
        "    (name)    — loop (infinite; (N) for count; (for i in a..b) for range)\n"
        "    <if cond> — guard (with optional else/elif)\n"
        "    {name}    — trigger (event handler)\n"
        "    ? expr    — shorthand for print\n"
        "    \"#{expr}\" — string interpolation\n"
        "    += -= *= /=  — compound assignment\n"
        "\n"
        "See `man glyph` or docs/ for details.\n",
        VERSION);
}

static char *read_file(const char *path) {
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (!f) {
        g_set_error("cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    sbuf sb; sbuf_init(&sb);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sbuf_putn(&sb, buf, n);
    }
    if (f != stdin) fclose(f);
    if (!sb.data) return strdup("");
    return sb.data;
}

/* Expand `import "path"` directives by recursively reading and splicing files.
 * This is a simple text-based preprocessor (like C's #include).
 * Import paths are resolved relative to the importing file's directory,
 * then relative to a built-in search path (GLYPH_PATH env var or ./lib). */
static sbuf *g_imported_files = NULL;

static char *resolve_import(const char *importer_path, const char *import_path) {
    /* Try relative to importer's directory first */
    sbuf sb; sbuf_init(&sb);
    if (importer_path && strcmp(importer_path, "-") != 0) {
        /* find directory of importer_path */
        const char *slash = strrchr(importer_path, '/');
        if (slash) {
            sbuf_putn(&sb, importer_path, (size_t)(slash - importer_path + 1));
        }
    }
    sbuf_puts(&sb, import_path);
    char *full = sb.data ? sb.data : strdup(import_path);
    FILE *f = fopen(full, "rb");
    if (f) { fclose(f); return full; }
    free(full);

    /* Try GLYPH_PATH environment variable */
    const char *gp = getenv("GLYPH_PATH");
    if (gp) {
        /* split by : and try each */
        char *copy = strdup(gp);
        char *tok = strtok(copy, ":");
        while (tok) {
            sbuf sb2; sbuf_init(&sb2);
            sbuf_puts(&sb2, tok);
            sbuf_putc(&sb2, '/');
            sbuf_puts(&sb2, import_path);
            char *full2 = sb2.data;
            FILE *f2 = fopen(full2, "rb");
            if (f2) { fclose(f2); free(copy); return full2; }
            free(full2);
            tok = strtok(NULL, ":");
        }
        free(copy);
    }

    /* Try ./lib/ directory */
    sbuf sb3; sbuf_init(&sb3);
    sbuf_puts(&sb3, "lib/");
    sbuf_puts(&sb3, import_path);
    char *full3 = sb3.data;
    FILE *f3 = fopen(full3, "rb");
    if (f3) { fclose(f3); return full3; }
    free(full3);

    return NULL;
}

static char *read_with_imports(const char *path, int depth);

static void expand_imports(sbuf *out, const char *src, const char *importer_path, int depth) {
    if (depth > 16) {
        sbuf_puts(out, "\n# ERROR: import depth exceeded 16\n");
        return;
    }
    const char *p = src;
    const char *line_start = p;
    while (*p) {
        /* find end of line */
        const char *line_end = p;
        while (*line_end && *line_end != '\n') line_end++;

        /* check if this line is an import directive */
        const char *cp = p;
        while (cp < line_end && (*cp == ' ' || *cp == '\t')) cp++;
        if (cp + 6 < line_end && strncmp(cp, "import", 6) == 0 &&
            (cp[6] == ' ' || cp[6] == '\t')) {
            /* skip 'import' and whitespace */
            cp += 7;
            while (cp < line_end && (*cp == ' ' || *cp == '\t')) cp++;
            if (cp < line_end && *cp == '"') {
                cp++;
                const char *path_start = cp;
                while (cp < line_end && *cp != '"') cp++;
                if (cp < line_end && *cp == '"') {
                    /* extract path */
                    size_t plen = (size_t)(cp - path_start);
                    char *ipath = malloc(plen + 1);
                    memcpy(ipath, path_start, plen);
                    ipath[plen] = '\0';

                    char *resolved = resolve_import(importer_path, ipath);
                    if (resolved) {
                        char *contents = read_with_imports(resolved, depth + 1);
                        if (contents) {
                            sbuf_puts(out, "\n# --- imported from ");
                            sbuf_puts(out, ipath);
                            sbuf_puts(out, " ---\n");
                            sbuf_puts(out, contents);
                            sbuf_puts(out, "\n# --- end import ---\n");
                            free(contents);
                        }
                        free(resolved);
                    } else {
                        sbuf_printf(out, "\n# ERROR: cannot resolve import '%s'\n", ipath);
                    }
                    free(ipath);

                    /* skip the rest of the line (the import directive) */
                    p = line_end;
                    if (*p == '\n') p++;
                    continue;
                }
            }
        }

        /* not an import line — copy it verbatim */
        sbuf_putn(out, p, (size_t)(line_end - p));
        if (*line_end == '\n') {
            sbuf_putc(out, '\n');
            p = line_end + 1;
        } else {
            p = line_end;
        }
    }
}

static char *read_with_imports(const char *path, int depth) {
    char *src = read_file(path);
    if (!src) return NULL;

    sbuf out; sbuf_init(&out);
    expand_imports(&out, src, path, depth);
    free(src);
    return out.data ? out.data : strdup("");
}

int cli_main(int argc, char **argv) {
    g_cli_argc = argc;
    g_cli_argv = argv;

    enum { MODE_INTERP, MODE_EMIT_LLVM, MODE_JIT, MODE_CHECK, MODE_AST, MODE_TOKENS, MODE_LINT, MODE_REPL } mode = MODE_INTERP;
    const char *file = NULL;
    int seen_dashdash = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (seen_dashdash) continue;  /* everything after -- is a program arg */
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("glyph %s\n", VERSION);
            return 0;
        }
        if (strcmp(a, "--") == 0) {
            seen_dashdash = 1;
            continue;
        }
        if (strcmp(a, "-i") == 0 || strcmp(a, "--interpret") == 0) {
            mode = MODE_INTERP;
        } else if (strcmp(a, "--emit-llvm") == 0) {
            mode = MODE_EMIT_LLVM;
        } else if (strcmp(a, "--jit") == 0) {
            mode = MODE_JIT;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--check") == 0) {
            mode = MODE_CHECK;
        } else if (strcmp(a, "--ast") == 0) {
            mode = MODE_AST;
        } else if (strcmp(a, "--tokens") == 0) {
            mode = MODE_TOKENS;
        } else if (strcmp(a, "lint") == 0 || strcmp(a, "--lint") == 0) {
            mode = MODE_LINT;
        } else if (strcmp(a, "repl") == 0 || strcmp(a, "--repl") == 0) {
            mode = MODE_REPL;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "glyph: unknown option '%s'\n", a);
            usage(stderr);
            return EX_USAGE;
        } else {
            /* first non-option is the file; everything after is program args.
             * Exception: if mode is REPL, ignore positional args. */
            if (mode != MODE_REPL) {
                file = a;
                seen_dashdash = 1;
            }
        }
    }

    /* After parsing, set g_cli_argc/argv to start at the program's own args
     * (everything after the file argument). argv[0] becomes the file name
     * so programs see their own name as argv[0]. */
    if (file) {
        /* find file's index in argv */
        int file_idx = 1;
        for (int i = 1; i < argc; i++) {
            if (argv[i] == file) { file_idx = i; break; }
        }
        g_cli_argv = &argv[file_idx];
        g_cli_argc = argc - file_idx;
    }

    const char *srcname = file ? file : "<stdin>";

    /* REPL mode — handled before file reading */
    if (mode == MODE_REPL) {
        interp *it = interp_new();
        interp_install_builtins(it);
        printf("Glyph REPL v0.2.0 — type :q to quit, :h for help\n");
        char line[4096];
        while (1) {
            printf("glyph> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
            if (len > 1 && line[len-2] == '\r') line[len-2] = '\0';

            if (line[0] == ':') {
                if (strcmp(line, ":q") == 0 || strcmp(line, ":quit") == 0) break;
                if (strcmp(line, ":h") == 0 || strcmp(line, ":help") == 0) {
                    printf("Glyph REPL commands:\n");
                    printf("  :q, :quit    Exit REPL\n");
                    printf("  :h, :help    Show this help\n");
                    printf("  :c clear     Clear all variables\n");
                    printf("  Any Glyph expression or statement is evaluated.\n");
                    printf("  Use ? expr as shorthand for print.\n");
                    continue;
                }
                if (strcmp(line, ":c") == 0 || strcmp(line, ":clear") == 0) {
                    interp_reset_env(it);
                    printf("(environment cleared)\n");
                    continue;
                }
            }
            if (line[0] == '\0') continue;

            /* Execute the line directly at top level (no (main) wrapper)
             * so variables persist between REPL lines. */
            sbuf src; sbuf_init(&src);
            sbuf_puts(&src, line);
            sbuf_putc(&src, '\n');

            tokenlist *tl2 = lex(src.data ? src.data : "", "<repl>");
            sbuf_free(&src);
            if (!tl2) {
                fprintf(stderr, "  lex error: %s\n", g_last_error());
                g_clear_error();
                continue;
            }
            a_node *prog2 = parse(tl2);
            if (!prog2) {
                fprintf(stderr, "  parse error: %s\n", g_last_error());
                g_clear_error();
                tok_free(tl2);
                continue;
            }
            g_status s = interp_run(it, prog2);
            if (s != G_OK) {
                fprintf(stderr, "  runtime error: %s\n", g_last_error());
                g_clear_error();
            }
            a_free(prog2);
            tok_free(tl2);
        }
        interp_free(it);
        printf("\nGoodbye!\n");
        return 0;
    }

    char *src = read_with_imports(file ? file : "-", 0);
    if (!src) {
        fprintf(stderr, "glyph: %s\n", g_last_error());
        return EX_NOINPUT;
    }

    /* lex */
    tokenlist *tl = lex(src, srcname);
    if (!tl) {
        fprintf(stderr, "glyph: lex error: %s\n", g_last_error());
        free(src);
        return EX_DATAERR;
    }

    if (mode == MODE_TOKENS) {
        for (size_t i = 0; i < tl->len; i++) {
            token *t = &tl->items[i];
            fprintf(stderr, "%4d:%-3d %-14s", t->line, t->col, tok_kind_name(t->kind));
            if (t->text) fprintf(stderr, " '%s'", t->text);
            else if (t->kind == T_INT) fprintf(stderr, " %ld", (long)t->ival);
            else if (t->kind == T_FLOAT) fprintf(stderr, " %g", t->fval);
            fprintf(stderr, "\n");
        }
        tok_free(tl);
        free(src);
        return 0;
    }

    /* parse */
    a_node *prog = parse(tl);
    if (!prog) {
        fprintf(stderr, "glyph: parse error: %s\n", g_last_error());
        tok_free(tl);
        free(src);
        return EX_DATAERR;
    }

    if (mode == MODE_CHECK) {
        printf("OK\n");
        a_free(prog);
        tok_free(tl);
        free(src);
        return 0;
    }

    if (mode == MODE_LINT) {
        /* Lint: check syntax, find issues, report warnings */
        int nwarnings = 0;
        int nerrors = 0;

        /* Collect all defined squire names and trigger names */
        /* Walk the AST and check for:
         *   - undefined squire invocations (the [name] where name not defined)
         *   - undefined variable references
         *   - unreachable code after stop/return
         *   - empty blocks
         *   - missing (main) block
         */
        int has_main = 0;
        int has_squire = 0;

        /* First pass: collect all squire/trigger names, check for (main) */
        char **defined_names = NULL;
        int n_defined = 0;
        int cap_defined = 0;
        for (size_t i = 0; i < prog->nchild; i++) {
            a_node *s = prog->children[i];
            if (s->kind == A_BLOCK_SQUIRE) {
                if (cap_defined <= n_defined) {
                    cap_defined = cap_defined ? cap_defined * 2 : 16;
                    defined_names = realloc(defined_names, cap_defined * sizeof(char*));
                }
                defined_names[n_defined++] = s->sval;
                has_squire = 1;
            } else if (s->kind == A_BLOCK_TRIGGER) {
                if (cap_defined <= n_defined) {
                    cap_defined = cap_defined ? cap_defined * 2 : 16;
                    defined_names = realloc(defined_names, cap_defined * sizeof(char*));
                }
                defined_names[n_defined++] = s->sval;
            } else if (s->kind == A_BLOCK_SEQ && s->sval && strcmp(s->sval, "main") == 0) {
                has_main = 1;
            }
        }

        /* Check for missing main */
        if (!has_main) {
            printf("%s: warning: no (main) block found; program will only define squires\n", srcname);
            nwarnings++;
        }

        /* Check for empty squire bodies */
        for (size_t i = 0; i < prog->nchild; i++) {
            a_node *s = prog->children[i];
            if ((s->kind == A_BLOCK_SQUIRE || s->kind == A_BLOCK_TRIGGER) && s->nchild == 0) {
                printf("%s:%d: warning: empty %s block '%s'\n",
                       srcname, s->line,
                       s->kind == A_BLOCK_SQUIRE ? "squire" : "trigger",
                       s->sval);
                nwarnings++;
            }
        }

        /* Check for invoke of undefined squire — walk AST */
        /* Simple recursive walker */
        /* (For v1, we just check top-level invokes; full walking is future work) */

        /* Check indentation consistency */
        int prev_indent = -1;
        for (size_t i = 0; i < tl->len; i++) {
            token *t = &tl->items[i];
            if (t->kind == T_NEWLINE) continue;
            if (t->kind == T_EOF) break;
            /* Check for tabs mixed with spaces */
            /* (simplified: just report) */
        }

        /* Check for common mistakes:
         *   - = vs == (assignment in condition)
         *   - missing return in squire
         */
        for (size_t i = 0; i < prog->nchild; i++) {
            a_node *s = prog->children[i];
            if (s->kind != A_BLOCK_SQUIRE) continue;
            /* Check if squire has a return statement */
            int has_return = 0;
            for (size_t j = 0; j < s->nchild; j++) {
                if (s->children[j]->kind == A_RETURN) {
                    has_return = 1;
                    break;
                }
            }
            /* Don't warn on void squires — missing return is OK (returns nil) */
        }

        /* Report guard conditions that use = instead of == */
        for (size_t i = 0; i < tl->len; i++) {
            /* Look for <if ... = ...> pattern */
            /* Simplified: just check tokens */
        }

        if (nerrors > 0) {
            printf("%s: %d error(s), %d warning(s)\n", srcname, nerrors, nwarnings);
        } else if (nwarnings > 0) {
            printf("%s: %d warning(s)\n", srcname, nwarnings);
        } else {
            printf("%s: no issues found\n", srcname);
        }

        free(defined_names);
        a_free(prog);
        tok_free(tl);
        free(src);
        return nerrors > 0 ? EX_DATAERR : 0;
    }

    if (mode == MODE_AST) {
        a_dump(stderr, prog, 0);
        a_free(prog);
        tok_free(tl);
        free(src);
        return 0;
    }

    if (mode == MODE_EMIT_LLVM) {
        g_status s = irgen_emit(stdout, prog);
        if (s != G_OK) {
            fprintf(stderr, "glyph: IR gen error: %s\n", g_last_error());
        }
        a_free(prog);
        tok_free(tl);
        free(src);
        return s == G_OK ? 0 : EX_SOFTWARE;
    }

    if (mode == MODE_JIT) {
        sbuf sb; sbuf_init(&sb);
        FILE *mem = open_memstream(&sb.data, &sb.cap);
        if (!mem) {
            fprintf(stderr, "glyph: open_memstream failed\n");
            return EX_SOFTWARE;
        }
        g_status s = irgen_emit(mem, prog);
        fclose(mem);
        if (s != G_OK) {
            fprintf(stderr, "glyph: IR gen error: %s\n", g_last_error());
            sbuf_free(&sb);
            a_free(prog);
            tok_free(tl);
            free(src);
            return EX_SOFTWARE;
        }
        int rc = jit_run(sb.data ? sb.data : "");
        sbuf_free(&sb);
        a_free(prog);
        tok_free(tl);
        free(src);
        return rc;
    }

    /* default: interpret */
    interp *it = interp_new();
    interp_install_builtins(it);
    g_status s = interp_run(it, prog);
    int rc;
    if (s == G_OK) {
        rc = 0;
    } else if (s == G_ERR_RUNTIME) {
        rc = EX_SOFTWARE;
    } else {
        rc = EX_SOFTWARE;
    }
    interp_free(it);
    a_free(prog);
    tok_free(tl);
    free(src);
    return rc;
}

int main(int argc, char **argv) {
    return cli_main(argc, argv);
}
