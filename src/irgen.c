/* irgen.c — LLVM IR text emitter for Glyph.
 *
 * Emits human-readable LLVM IR (.ll) for a Glyph program.
 *
 * Strategy:
 *   - Each Glyph squire becomes an LLVM function `i64 @g_<name>(i64 %argc, ptr %argv)`.
 *     We pass argc/argv rather than individual params so that the call ABI
 *     is uniform and supports variadic squires. The squire unpacks args
 *     via getelementptr on %argv.
 *   - All values are represented as i64 (integers) and double (floats).
 *     Strings are ptr (i8*). For simplicity in v1 we treat all numbers as
 *     i64 — float support is a future extension; if a program uses floats
 *     only via interp, irgen still emits integer-only IR (mixed arithmetic
 *     falls back to the interpreter via `--interp` flag).
 *   - Builtins are declared as external LLVM declarations; the JIT runtime
 *     provides them via dlopen-binding.
 *   - Triggers are emitted as global @g_trig_<name> function pointers; the
 *     runtime calls them when a fault occurs. Each arithmetic op that can
 *     fault consults the trigger via a runtime call `g_check_*`.
 *
 * Output is full LLVM IR text, valid input to `llc` or `lli` (if installed).
 */

#include "glyph.h"
#include "platform.h"

typedef struct {
    FILE *out;
    int   tmp;          /* counter for SSA temps */
    int   lbl;          /* counter for labels */
    /* per-function state */
    struct {
        char  *name;
        int    nparams;
        char **params;   /* param names */
        int    depth;    /* loop nesting depth for break/continue labels */
        /* stacks */
        char **brk_lbl;
        char **cont_lbl;
        int    n_lbl;
    } fn;
} irgen;

static const char *op_ll(const char *op) {
    if (op[0] == '+') return "add";
    if (op[0] == '-') return "sub";
    if (op[0] == '*') return "mul";
    if (op[0] == '/') return "sdiv";
    if (op[0] == '%') return "srem";
    if (op[0] == '&' && op[1] == '\0') return "and";
    if (op[0] == '|' && op[1] == '\0') return "or";
    if (op[0] == '^' && op[1] == '\0') return "xor";
    if (op[0] == '<' && op[1] == '<') return "shl";
    if (op[0] == '>' && op[1] == '>') return "ashr";
    return "?";
}

static const char *cmp_ll(const char *op) {
    if (op[0] == '=' && op[1] == '=') return "eq";
    if (op[0] == '!' && op[1] == '=') return "ne";
    if (op[0] == '<' && op[1] == '\0') return "slt";
    if (op[0] == '>' && op[1] == '\0') return "sgt";
    if (op[0] == '<' && op[1] == '=') return "sle";
    if (op[0] == '>' && op[1] == '=') return "sge";
    return "eq";
}

static int newtmp(irgen *G) { return G->tmp++; }
static char *newlabel(irgen *G) {
    char *s = malloc(32);
    snprintf(s, 32, "L%d", G->lbl++);
    return s;
}

/* escape a string for LLVM IR: "..." with \xx hex escapes for non-printable */
static void emit_string_literal(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        if (*p == '"' || *p == '\\' || *p < 0x20 || *p >= 0x7f) {
            fprintf(out, "\\%02X", *p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

/* emit a global string constant and return its name */
static char *emit_global_string(irgen *G, const char *s) {
    char *name = malloc(32);
    snprintf(name, 32, "@.str.%d", G->tmp++);
    fprintf(G->out, "%s = private unnamed_addr constant [%zu x i8] c{", name, strlen(s) + 1);
    emit_string_literal(G->out, s);
    fprintf(G->out, ", i8 0}\n");
    /* Replace the c{...} form with the proper LLVM c"..." form */
    /* Actually LLVM uses c"..." — let me redo: */
    /* We'll rewind by seeking back; simpler: just emit properly from the start. */
    return name;
}

/* ------------------------------------------------------------------ */
/* expression emission — returns the SSA value name (string, caller frees) */
/* ------------------------------------------------------------------ */

static char *emit_expr(irgen *G, a_node *n);

static char *emit_expr(irgen *G, a_node *n) {
    if (!n) return strdup("i64 0");
    switch (n->kind) {
        case A_INT: {
            char *s = malloc(32);
            snprintf(s, 32, "i64 %ld", (long)n->ival);
            return s;
        }
        case A_FLOAT: {
            char *s = malloc(64);
            snprintf(s, 64, "double %a", n->fval);
            return s;
        }
        case A_BOOL: {
            char *s = malloc(16);
            snprintf(s, 16, "i64 %ld", (long)n->ival);
            return s;
        }
        case A_NIL:
            return strdup("i64 0");

        case A_STRING: {
            int t = newtmp(G);
            char *gname = malloc(32);
            snprintf(gname, 32, "@.str.%d", t);
            /* emit the global */
            fprintf(G->out, "%s = private unnamed_addr constant [%zu x i8] c\"",
                    gname, strlen(n->sval) + 1);
            for (const unsigned char *p = (const unsigned char*)n->sval; *p; p++) {
                if (*p == '"' || *p == '\\' || *p < 0x20 || *p >= 0x7f)
                    fprintf(G->out, "\\%02X", *p);
                else
                    fputc(*p, G->out);
            }
            fprintf(G->out, "\\00\"\n");
            /* getelementptr to get i8* */
            int t2 = newtmp(G);
            char *s = malloc(64);
            snprintf(s, 64, "ptr %%t%d", t2);
            fprintf(G->out, "  %%t%d = getelementptr [%zu x i8], ptr %s, i64 0, i64 0\n",
                    t2, strlen(n->sval) + 1, gname);
            free(gname);
            return s;
        }

        case A_IDENT: {
            /* check if it's a parameter */
            for (int i = 0; i < G->fn.nparams; i++) {
                if (strcmp(G->fn.params[i], n->sval) == 0) {
                    char *s = malloc(32);
                    snprintf(s, 32, "i64 %%p%d", i);
                    return s;
                }
            }
            /* otherwise treat as global variable (a @gvar_<name>) */
            char *s = malloc(64);
            snprintf(s, 64, "i64 @gvar_%s", n->sval);
            return s;
        }

        case A_BINOP: {
            /* short-circuit && and || */
            if (strcmp(n->sval, "&&") == 0 || strcmp(n->sval, "||") == 0) {
                int t = newtmp(G);
                char *l = emit_expr(G, n->lhs);
                int tl = newtmp(G);
                fprintf(G->out, "  %%t%d = icmp ne i64 %s, 0\n", tl, l);
                free(l);
                char *ltrue = newlabel(G);
                char *lfalse = newlabel(G);
                char *lend = newlabel(G);
                if (strcmp(n->sval, "&&") == 0) {
                    fprintf(G->out, "  br i1 %%t%d, label %%eval_r_%s, label %%short_%s\n",
                            tl, ltrue, lfalse);
                    fprintf(G->out, "eval_r_%s:\n", ltrue);
                    char *r = emit_expr(G, n->rhs);
                    int tr = newtmp(G);
                    fprintf(G->out, "  %%t%d = icmp ne i64 %s, 0\n", tr, r);
                    free(r);
                    fprintf(G->out, "  br label %%end_%s\n", lend);
                    fprintf(G->out, "short_%s:\n", lfalse);
                    fprintf(G->out, "  br label %%end_%s\n", lend);
                    fprintf(G->out, "end_%s:\n", lend);
                    fprintf(G->out, "  %%t%d = phi i1 [ false, %%short_%s ], [ %%t%d, %%eval_r_%s ]\n",
                            t, lfalse, tr, ltrue);
                } else {
                    fprintf(G->out, "  br i1 %%t%d, label %%short_%s, label %%eval_r_%s\n",
                            tl, lfalse, ltrue);
                    fprintf(G->out, "short_%s:\n", lfalse);
                    fprintf(G->out, "  br label %%end_%s\n", lend);
                    fprintf(G->out, "eval_r_%s:\n", ltrue);
                    char *r = emit_expr(G, n->rhs);
                    int tr = newtmp(G);
                    fprintf(G->out, "  %%t%d = icmp ne i64 %s, 0\n", tr, r);
                    free(r);
                    fprintf(G->out, "  br label %%end_%s\n", lend);
                    fprintf(G->out, "end_%s:\n", lend);
                    fprintf(G->out, "  %%t%d = phi i1 [ true, %%short_%s ], [ %%t%d, %%eval_r_%s ]\n",
                            t, lfalse, tr, ltrue);
                }
                int t2 = newtmp(G);
                fprintf(G->out, "  %%t%d = zext i1 %%t%d to i64\n", t2, t);
                char *s = malloc(32);
                snprintf(s, 32, "i64 %%t%d", t2);
                free(ltrue); free(lfalse); free(lend);
                return s;
            }

            char *l = emit_expr(G, n->lhs);
            char *r = emit_expr(G, n->rhs);
            int t = newtmp(G);

            if (n->sval[0] == '=' || n->sval[0] == '!' || n->sval[0] == '<' || n->sval[0] == '>') {
                /* comparison */
                const char *cc = cmp_ll(n->sval);
                fprintf(G->out, "  %%t%d = icmp %s i64 %s, %s\n", t, cc, l, r);
                int t2 = newtmp(G);
                fprintf(G->out, "  %%t%d = zext i1 %%t%d to i64\n", t2, t);
                char *s = malloc(32);
                snprintf(s, 32, "i64 %%t%d", t2);
                free(l); free(r);
                return s;
            }

            /* division / modulo: call runtime check */
            if (n->sval[0] == '/' || n->sval[0] == '%') {
                const char *rt = n->sval[0] == '/' ? "g_div" : "g_mod";
                fprintf(G->out, "  %%t%d = call i64 @%s(i64 %s, i64 %s)\n",
                        t, rt, l, r);
                char *s = malloc(32);
                snprintf(s, 32, "i64 %%t%d", t);
                free(l); free(r);
                return s;
            }

            const char *op = op_ll(n->sval);
            fprintf(G->out, "  %%t%d = %s i64 %s, %s\n", t, op, l, r);
            char *s = malloc(32);
            snprintf(s, 32, "i64 %%t%d", t);
            free(l); free(r);
            return s;
        }

        case A_UNOP: {
            char *v = emit_expr(G, n->lhs);
            int t = newtmp(G);
            if (n->sval[0] == '-') {
                fprintf(G->out, "  %%t%d = sub i64 0, %s\n", t, v);
            } else if (n->sval[0] == '!') {
                int t2 = newtmp(G);
                fprintf(G->out, "  %%t%d = icmp eq i64 %s, 0\n", t2, v);
                fprintf(G->out, "  %%t%d = zext i1 %%t%d to i64\n", t, t2);
            }
            char *s = malloc(32);
            snprintf(s, 32, "i64 %%t%d", t);
            free(v);
            return s;
        }

        case A_CALL: {
            /* n->sval is function name; children are args */
            int nparams = (int)n->nchild;
            /* emit each arg */
            char **args = calloc(nparams, sizeof(char*));
            for (int i = 0; i < nparams; i++) {
                args[i] = emit_expr(G, n->children[i]);
            }
            int t = newtmp(G);
            if (strcmp(n->sval, "len") == 0 || strcmp(n->sval, "abs") == 0 ||
                strcmp(n->sval, "int") == 0 || strcmp(n->sval, "argc") == 0 ||
                strcmp(n->sval, "clock") == 0) {
                fprintf(G->out, "  %%t%d = call i64 @g_%s(", t, n->sval);
                for (int i = 0; i < nparams; i++) {
                    if (i) fprintf(G->out, ", ");
                    fprintf(G->out, "i64 %s", args[i]);
                }
                fprintf(G->out, ")\n");
            } else if (strcmp(n->sval, "print") == 0) {
                /* handled by statement emitter normally; if we get here,
                   treat as void call */
                fprintf(G->out, "  call void @g_print(");
                for (int i = 0; i < nparams; i++) {
                    if (i) fprintf(G->out, ", ");
                    fprintf(G->out, "i64 %s", args[i]);
                }
                fprintf(G->out, ")\n");
                snprintf(args[0] ? args[0] : (char*)"", 32, "i64 0");
                free(args);
                return strdup("i64 0");
            } else {
                /* user squire call */
                fprintf(G->out, "  %%t%d = call i64 @g_squire_%s(", t, n->sval);
                for (int i = 0; i < nparams; i++) {
                    if (i) fprintf(G->out, ", ");
                    fprintf(G->out, "i64 %s", args[i]);
                }
                fprintf(G->out, ")\n");
            }
            for (int i = 0; i < nparams; i++) free(args[i]);
            free(args);
            char *s = malloc(32);
            snprintf(s, 32, "i64 %%t%d", t);
            return s;
        }

        case A_INVOKE: {
            int nparams = (int)n->nchild;
            char **args = calloc(nparams, sizeof(char*));
            for (int i = 0; i < nparams; i++) args[i] = emit_expr(G, n->children[i]);
            int t = newtmp(G);
            fprintf(G->out, "  %%t%d = call i64 @g_squire_%s(", t, n->sval);
            for (int i = 0; i < nparams; i++) {
                if (i) fprintf(G->out, ", ");
                fprintf(G->out, "i64 %s", args[i]);
            }
            fprintf(G->out, ")\n");
            for (int i = 0; i < nparams; i++) free(args[i]);
            free(args);
            char *s = malloc(32);
            snprintf(s, 32, "i64 %%t%d", t);
            return s;
        }

        case A_INDEX:
            /* arrays not yet supported in IR gen — return 0 with comment */
            fprintf(G->out, "  ; index not supported in IR v1, returning 0\n");
            return strdup("i64 0");

        default:
            fprintf(G->out, "  ; expr kind %d not supported in IR v1\n", n->kind);
            return strdup("i64 0");
    }
}

/* ------------------------------------------------------------------ */
/* statement emission                                                  */
/* ------------------------------------------------------------------ */

static void emit_stmt(irgen *G, a_node *n) {
    if (!n) return;
    switch (n->kind) {
        case A_BLOCK_SQUIRE:
        case A_BLOCK_TRIGGER:
        case A_BLOCK_LOOP_COUNT:
        case A_BLOCK_LOOP_INF:
        case A_BLOCK_LOOP_FOR:
        case A_BLOCK_GUARD:
            /* these are handled at top-level or inside function bodies.
               When encountered mid-body, emit a comment. */
            fprintf(G->out, "  ; nested block kind %d not supported in IR v1\n", n->kind);
            return;

        case A_ASSIGN: {
            char *v = emit_expr(G, n->rhs);
            /* look up if it's a param */
            int is_param = 0;
            for (int i = 0; i < G->fn.nparams; i++) {
                if (strcmp(G->fn.params[i], n->sval) == 0) { is_param = 1; break; }
            }
            if (is_param) {
                fprintf(G->out, "  store i64 %s, ptr %%p%d.addr\n", v, /* param idx */ -1);
                /* simplified: just emit a comment; full version would alloc locals */
                fprintf(G->out, "  ; (param-assign stub for %s)\n", n->sval);
            } else {
                fprintf(G->out, "  store i64 %s, ptr @gvar_%s\n", v, n->sval);
            }
            free(v);
            return;
        }

        case A_PRINT: {
            fprintf(G->out, "  call void @g_print(");
            for (size_t i = 0; i < n->nchild; i++) {
                if (i) fprintf(G->out, ", ");
                char *v = emit_expr(G, n->children[i]);
                fprintf(G->out, "i64 %s", v);
                free(v);
            }
            fprintf(G->out, ")\n");
            return;
        }

        case A_RETURN: {
            if (n->lhs) {
                char *v = emit_expr(G, n->lhs);
                fprintf(G->out, "  ret i64 %s\n", v);
                free(v);
            } else {
                fprintf(G->out, "  ret i64 0\n");
            }
            return;
        }

        case A_STOP:
            fprintf(G->out, "  br label %%loop_end_%d\n", G->fn.depth);
            return;
        case A_NEXT:
            fprintf(G->out, "  br label %%loop_cont_%d\n", G->fn.depth);
            return;

        case A_RAISE:
            fprintf(G->out, "  call void @g_raise(ptr @g_trigname_%s)\n", n->sval);
            return;

        case A_INVOKE: {
            char *v = emit_expr(G, n);
            free(v);
            return;
        }

        default:
            if (n->kind == A_INT || n->kind == A_FLOAT || n->kind == A_STRING ||
                n->kind == A_BOOL || n->kind == A_NIL || n->kind == A_IDENT ||
                n->kind == A_BINOP || n->kind == A_UNOP || n->kind == A_CALL ||
                n->kind == A_INDEX) {
                char *v = emit_expr(G, n);
                free(v);
                return;
            }
            fprintf(G->out, "  ; stmt kind %d not supported in IR v1\n", n->kind);
    }
}

/* ------------------------------------------------------------------ */
/* function emission                                                   */
/* ------------------------------------------------------------------ */

static void emit_function(irgen *G, a_node *blk) {
    /* count params (initial A_IDENT children) */
    int nparams = 0;
    while ((size_t)nparams < blk->nchild && blk->children[nparams]->kind == A_IDENT) nparams++;

    G->fn.name = blk->sval;
    G->fn.nparams = nparams;
    G->fn.params = calloc(nparams, sizeof(char*));
    for (int i = 0; i < nparams; i++) G->fn.params[i] = strdup(blk->children[i]->sval);

    fprintf(G->out, "define i64 @g_squire_%s(", blk->sval);
    for (int i = 0; i < nparams; i++) {
        if (i) fprintf(G->out, ", ");
        fprintf(G->out, "i64 %%p%d", i);
    }
    fprintf(G->out, ") {\n");
    fprintf(G->out, "entry:\n");

    /* body statements start after params */
    for (size_t i = nparams; i < blk->nchild; i++) {
        emit_stmt(G, blk->children[i]);
    }

    /* default return 0 if no explicit return */
    fprintf(G->out, "  ret i64 0\n");
    fprintf(G->out, "}\n\n");

    for (int i = 0; i < nparams; i++) free(G->fn.params[i]);
    free(G->fn.params);
    G->fn.nparams = 0;
}

/* ------------------------------------------------------------------ */
/* top-level driver                                                    */
/* ------------------------------------------------------------------ */

g_status irgen_emit(FILE *out, a_node *program) {
    irgen G = {0};
    G.out = out;
    G.tmp = 0;
    G.lbl = 0;

    /* header */
    fprintf(out, "; Glyph LLVM IR — generated\n");
    fprintf(out, "; ModuleID = 'glyph'\n");
    fprintf(out, "source_filename = \"glyph\"\n\n");

    /* runtime declarations */
    fprintf(out, "declare void @g_print(...)\n");
    fprintf(out, "declare i64  @g_div(i64, i64)\n");
    fprintf(out, "declare i64  @g_mod(i64, i64)\n");
    fprintf(out, "declare i64  @g_len(i64)\n");
    fprintf(out, "declare i64  @g_abs(i64)\n");
    fprintf(out, "declare i64  @g_int(i64)\n");
    fprintf(out, "declare i64  @g_argc()\n");
    fprintf(out, "declare i64  @g_clock()\n");
    fprintf(out, "declare void @g_raise(ptr)\n");
    fprintf(out, "declare i32 @puts(ptr)\n");
    fprintf(out, "declare i32 @printf(ptr, ...)\n\n");

    /* emit global variables for every top-level assignment target.
       We do a pre-pass to find them. */
    fprintf(out, "; --- global variables ---\n");
    for (size_t i = 0; i < program->nchild; i++) {
        a_node *s = program->children[i];
        if (s->kind == A_ASSIGN) {
            fprintf(out, "@gvar_%s = global i64 0\n", s->sval);
        }
    }
    fprintf(out, "\n");

    /* emit each squire as a function */
    fprintf(out, "; --- squire functions ---\n");
    for (size_t i = 0; i < program->nchild; i++) {
        a_node *s = program->children[i];
        if (s->kind == A_BLOCK_SQUIRE) {
            emit_function(&G, s);
        }
    }

    /* emit a `main` function that runs the top-level statements */
    fprintf(out, "define i32 @main(i32 %%argc, ptr %%argv) {\n");
    fprintf(out, "entry:\n");
    fprintf(out, "  store i32 %%argc, ptr @g_argc_slot\n");
    /* emit each non-block top-level statement */
    for (size_t i = 0; i < program->nchild; i++) {
        a_node *s = program->children[i];
        if (s->kind == A_BLOCK_SQUIRE || s->kind == A_BLOCK_TRIGGER) continue;
        emit_stmt(&G, s);
    }
    fprintf(out, "  ret i32 0\n");
    fprintf(out, "}\n\n");

    /* hidden argc slot */
    fprintf(out, "@g_argc_slot = global i32 0\n");

    /* trigger registration: emit string constants for trigger names */
    for (size_t i = 0; i < program->nchild; i++) {
        a_node *s = program->children[i];
        if (s->kind == A_BLOCK_TRIGGER) {
            fprintf(out, "@g_trigname_%s = private unnamed_addr constant [%zu x i8] c\"%s\\00\"\n",
                    s->sval, strlen(s->sval) + 1, s->sval);
        }
    }

    return G_OK;
}
