/* glyph.h — single umbrella header for the whole compiler.
 *
 * Pull this in to get every public type and function.
 *
 * The Glyph compiler is split into modules with strict Unix-style
 * boundaries:
 *
 *     src/lex.c     → tokens
 *     src/parse.c   → AST
 *     src/interp.c  → tree-walking interpreter
 *     src/irgen.c   → LLVM IR text emitter
 *     src/jit.c     → in-process JIT via dlopen(libLLVM)
 *     src/main.c    → CLI driver
 *
 * Each module owns its own state and exposes a small, opinionated API.
 */

#ifndef GLYPH_H
#define GLYPH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* error handling                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    G_OK = 0,
    G_ERR_LEX,
    G_ERR_PARSE,
    G_ERR_RUNTIME,
    G_ERR_TYPE,
    G_ERR_IO,
    G_ERR_INTERNAL,
    G_ERR_USAGE,
} g_status;

/* returns a thread-local human-readable message for the last error */
const char *g_last_error(void);
void        g_set_error(const char *fmt, ...);
void        g_clear_error(void);

/* ------------------------------------------------------------------ */
/* dynamic string (sbuf)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} sbuf;

void sbuf_init(sbuf *s);
void sbuf_free(sbuf *s);
void sbuf_putc(sbuf *s, char c);
void sbuf_puts(sbuf *s, const char *str);
void sbuf_printf(sbuf *s, const char *fmt, ...);
void sbuf_putn(sbuf *s, const char *data, size_t n);

/* ------------------------------------------------------------------ */
/* dynamic pointer array (pvec)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    void **data;
    size_t len;
    size_t cap;
} pvec;

void pvec_init(pvec *v);
void pvec_free(pvec *v);
void pvec_push(pvec *v, void *p);
void *pvec_pop(pvec *v);

/* ------------------------------------------------------------------ */
/* lexer                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    T_EOF = 0,
    T_NEWLINE, T_SEMI,
    T_IDENT, T_INT, T_FLOAT, T_STRING,
    T_SQUARE_O, T_SQUARE_C,   /* [ ]  — also used as expression index */
    T_PAREN_O,  T_PAREN_C,    /* ( )  */
    T_BRACE_O,  T_BRACE_C,    /* { }  */
    T_ANGLE_O,  T_ANGLE_C,    /* < >  — also < and > operators */
    /* keywords */
    T_KW_RETURN, T_KW_STOP, T_KW_NEXT, T_KW_RAISE,
    T_KW_PRINT, T_KW_THE, T_KW_IF, T_KW_ELSE, T_KW_ELIF, T_KW_WHEN,
    T_KW_FOR, T_KW_IN, T_KW_DOTDOT,
    T_KW_TRUE, T_KW_FALSE, T_KW_NIL,
    /* operators */
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
    T_ASSIGN, T_EQ, T_NE, T_LE, T_GE,
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
    T_AND, T_OR, T_BANG, T_AMP, T_PIPE, T_TILDE,
    T_SHL, T_SHR,
    T_LT, T_GT,           /* comparison operators (always) */
    T_COMMA,
    T_QUESTION,           /* ? — shorthand for print */
    /* context marker: this token begins a statement, lexer doesn't emit it */
    T_INDENT, T_DEDENT,
} token_kind;

typedef struct {
    token_kind kind;
    /* source location for diagnostics */
    int line;
    int col;
    /* payload */
    char  *text;     /* identifier / string content (already unescaped) */
    int64_t ival;    /* integer literal */
    double   fval;    /* float literal */
} token;

typedef struct {
    token *items;
    size_t len, cap;
    int    nlines;
    const char *srcname;
} tokenlist;

tokenlist *lex(const char *src, const char *srcname);
void       tok_free(tokenlist *tl);
const char *tok_kind_name(token_kind k);

/* ------------------------------------------------------------------ */
/* AST                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    A_INT, A_FLOAT, A_STRING, A_BOOL, A_NIL,
    A_IDENT,
    A_BINOP, A_UNOP,
    A_CALL, A_INDEX,
    A_ARRAY_LITERAL,     /* [expr, expr, ...] */
    A_ASSIGN,
    A_COMPOUND_ASSIGN,   /* name += expr, name -= expr, etc. */
    A_INDEX_ASSIGN,      /* name[idx] = value */
    A_PRINT,
    A_RETURN, A_STOP, A_NEXT, A_RAISE,
    A_INVOKE,             /* the [name]  — call a squire */
    A_BLOCK_SQUIRE,       /* [name args...] body */
    A_BLOCK_LOOP_COUNT,   /* (N) body */
    A_BLOCK_LOOP_INF,     /* (loop) body, infinite */
    A_BLOCK_LOOP_FOR,     /* (for i in a..b) body */
    A_BLOCK_SEQ,          /* (name) body — labeled sequence, runs once */
    A_BLOCK_GUARD,        /* <cond> body [else body2] */
    A_BLOCK_TRIGGER,      /* {name} body */
    A_PROGRAM,            /* top-level: list of statements */
} a_kind;

typedef struct a_node a_node;
struct a_node {
    a_kind kind;
    int    line;
    int    col;

    /* literal payloads */
    int64_t  ival;
    double   fval;
    char    *sval;        /* string content, identifier name, op name, block name */

    /* generic child vector (covers: block body, args, call args, print args,
       guard then/else, program statements, binop children, etc.) */
    a_node **children;
    size_t   nchild;

    /* secondary payload for specific nodes */
    a_node *lhs;          /* binop.lhs, assign.target, call.func, index.base */
    a_node *rhs;          /* binop.rhs, assign.value */
    a_node *cond;         /* guard.cond, loop_for.count, loop_count.count */
    a_node *else_branch;  /* guard.else */

    /* for-loop range */
    char   *loop_var;     /* for i in ... */
    a_node *range_start;
    a_node *range_end;
    a_node *range_step;
};

a_node *a_new(a_kind k, int line, int col);
void    a_free(a_node *n);
void    a_addchild(a_node *parent, a_node *child);
void    a_dump(FILE *out, a_node *n, int depth);   /* pretty-print */

/* ------------------------------------------------------------------ */
/* parser                                                             */
/* ------------------------------------------------------------------ */

a_node *parse(const tokenlist *tl);

/* ------------------------------------------------------------------ */
/* values (runtime)                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    V_INT, V_FLOAT, V_STRING, V_ARRAY, V_NIL, V_FUNC, V_NATIVE, V_PTR,
} v_kind;

typedef struct value value;
typedef struct value (*native_fn)(int argc, value *argv);

/* array storage type — shared by reference */
typedef struct {
    value *items;
    size_t len, cap;
} varr;

struct value {
    v_kind kind;
    union {
        int64_t  i;
        double   f;
        char    *s;          /* owned by V_STRING */
        varr    *arr;        /* heap-allocated, not refcounted in v1 */
        struct {
            a_node *def;     /* A_BLOCK_SQUIRE */
            struct env *closure;
        } func;
        native_fn nat;
        void    *ptr;        /* V_PTR: raw C pointer for FFI */
    } as;
    int refcount;            /* reserved for future use */
};

value v_int(int64_t i);
value v_float(double f);
value v_str(const char *s);
value v_str_take(char *s);     /* takes ownership of s */
value v_arr(void);
value v_nil(void);
value v_native(native_fn fn);
value v_ptr(void *p);

void  v_free(value *v);
value v_clone(const value *v); /* shallow copy, strings are shared-but-owned */
value v_truthy(const value *v);
char *v_to_string(const value *v);   /* returns malloc'd string for print */
int   v_eq(const value *a, const value *b);
int   v_lt(const value *a, const value *b);

/* ------------------------------------------------------------------ */
/* environment (interpreter)                                          */
/* ------------------------------------------------------------------ */

typedef struct env {
    struct env *parent;
    char      **names;
    value      *vals;
    size_t      len, cap;
} env;

env  *env_new(env *parent);
void  env_free(env *e);
value env_get(env *e, const char *name, int *found);
int   env_set(env *e, const char *name, value v);   /* sets in nearest definer scope */
int   env_set_local(env *e, const char *name, value v); /* forces local */

/* ------------------------------------------------------------------ */
/* interpreter                                                        */
/* ------------------------------------------------------------------ */

typedef struct interp interp;

interp *interp_new(void);
void    interp_free(interp *it);
void    interp_register_trigger(interp *it, const char *name, a_node *body);
void    interp_install_builtins(interp *it);
g_status interp_run(interp *it, a_node *program);
void    interp_reset_env(interp *it);  /* clear all user variables, keep builtins */

/* signal hook (called by trap handler) */
void interp_signal(interp *it, int signo);

/* ------------------------------------------------------------------ */
/* LLVM IR text emitter                                               */
/* ------------------------------------------------------------------ */

/* emits LLVM IR text for the program into out. */
g_status irgen_emit(FILE *out, a_node *program);

/* ------------------------------------------------------------------ */
/* JIT (via dlopen of libLLVM-19.so)                                  */
/* ------------------------------------------------------------------ */

/* JIT-compiles the IR text and runs the `main` function. Returns exit code. */
int jit_run(const char *ir_text);

/* ------------------------------------------------------------------ */
/* CLI                                                                */
/* ------------------------------------------------------------------ */

int cli_main(int argc, char **argv);

/* ------------------------------------------------------------------ */
/* FFI extension (src/ffi.c)                                          */
/*                                                                    */
/* Language-agnostic FFI via subprocess pipes. Provides:              */
/*   exec(cmd, input?) -> string        Run cmd, return stdout         */
/*   exec_status(cmd, input?) -> int    Run cmd, return exit code      */
/*   pipe_open(cmd) -> ptr              Spawn persistent subprocess    */
/*   pipe_write(h, s) -> int            Write string to subprocess     */
/*   pipe_readln(h) -> string           Read one line (nil on EOF)     */
/*   pipe_read(h, n) -> string          Read up to n bytes             */
/*   pipe_close(h) -> int               Close subprocess, get status   */
/*   lang_eval(lang, code) -> string    Eval code in any language      */
/*   lang_call(lang, mod, fn, args)     Call fn in lang module         */
/*   lang_list() -> array               List available languages       */
/*                                                                    */
/* Each function is exposed as a getter returning the native_fn so    */
/* that interp.c (which owns the interp struct) can register them.    */
/* ------------------------------------------------------------------ */

native_fn ffi_nb_exec(void);
native_fn ffi_nb_exec_status(void);
native_fn ffi_nb_pipe_open(void);
native_fn ffi_nb_pipe_write(void);
native_fn ffi_nb_pipe_readln(void);
native_fn ffi_nb_pipe_read(void);
native_fn ffi_nb_pipe_close(void);
native_fn ffi_nb_lang_eval(void);
native_fn ffi_nb_lang_call(void);
native_fn ffi_nb_lang_list(void);

#endif /* GLYPH_H */
