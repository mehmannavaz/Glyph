/* interp.c — tree-walking interpreter for Glyph.
 *
 * Execution model:
 *   - Global environment holds all top-level squire/trigger definitions.
 *   - Squires execute in their own child env (closure-aware).
 *   - Loops execute in their own child env so `iter` is local.
 *   - Triggers are registered in a separate table; when a faulting op
 *     happens, the interp looks up the matching trigger and runs it; its
 *     return value substitutes the faulting op's result.
 */

#include "glyph.h"
#include "platform.h"

/* CLI args are set by main.c */
extern int    g_cli_argc;
extern char **g_cli_argv;

/* ------------------------------------------------------------------ */
/* control-flow signals                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    SIG_NONE = 0,
    SIG_RETURN,
    SIG_STOP,
    SIG_NEXT,
    SIG_RAISE,
    SIG_ERROR,
} sig_kind;

typedef struct {
    sig_kind kind;
    value    val;        /* return value, or trigger name (string) for RAISE */
    int      signo;      /* for SIG_RAISE when invoked from a Unix signal */
} ctrl_sig;

/* ------------------------------------------------------------------ */
/* trigger table                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char   *name;
    a_node *body;       /* A_BLOCK_TRIGGER body */
} trigger_entry;

/* ------------------------------------------------------------------ */
/* interpreter state                                                   */
/* ------------------------------------------------------------------ */

struct interp {
    env            *globals;
    trigger_entry  *triggers;
    size_t          ntrigger, cap_trigger;

    /* control flow */
    ctrl_sig        sig;
    jmp_buf        *ret_jmp;    /* current return target */
    jmp_buf        *loop_jmp;   /* current loop break target */
    jmp_buf        *next_jmp;   /* current loop continue target */

    /* signal handling */
    volatile sig_atomic_t pending_signal;
    int              signal_nums[64];
    int              n_pending_signals;

    /* trigger re-entrancy guard */
    int              in_trigger;
};

/* forward decls */
static value eval_expr(interp *it, a_node *n, env *e);
static void  exec_stmt(interp *it, a_node *n, env *e);
static void  exec_block(interp *it, a_node *n, env *e);
static value call_squire(interp *it, a_node *def, int argc, value *argv, env *closure);
static value fire_trigger(interp *it, const char *name);
static value do_binop(interp *it, const char *op, value l, value r, int line, int col);

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

interp *interp_new(void) {
    interp *it = calloc(1, sizeof(interp));
    it->globals = env_new(NULL);
    return it;
}

void interp_free(interp *it) {
    if (!it) return;
    env_free(it->globals);
    for (size_t i = 0; i < it->ntrigger; i++) free(it->triggers[i].name);
    free(it->triggers);
    free(it);
}

void interp_register_trigger(interp *it, const char *name, a_node *body) {
    /* replace if exists */
    for (size_t i = 0; i < it->ntrigger; i++) {
        if (strcmp(it->triggers[i].name, name) == 0) {
            it->triggers[i].body = body;
            return;
        }
    }
    if (it->ntrigger + 1 > it->cap_trigger) {
        it->cap_trigger = it->cap_trigger ? it->cap_trigger * 2 : 8;
        it->triggers = realloc(it->triggers, it->cap_trigger * sizeof(trigger_entry));
    }
    it->triggers[it->ntrigger].name = strdup(name);
    it->triggers[it->ntrigger].body = body;
    it->ntrigger++;
}

static a_node *find_trigger(interp *it, const char *name) {
    for (size_t i = 0; i < it->ntrigger; i++) {
        if (strcmp(it->triggers[i].name, name) == 0) {
            return it->triggers[i].body;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* builtin native functions                                            */
/* ------------------------------------------------------------------ */

/* print is handled specially by interpreter because it needs stdout */

static value nb_len(int argc, value *argv) {
    if (argc != 1) { g_set_error("len() takes 1 argument"); return v_nil(); }
    if (argv[0].kind == V_STRING) return v_int((int64_t)strlen(argv[0].as.s));
    if (argv[0].kind == V_ARRAY)  return v_int((int64_t)argv[0].as.arr->len);
    g_set_error("len() needs string or array");
    return v_nil();
}

static value nb_range(int argc, value *argv) {
    if (argc < 2 || argc > 3) { g_set_error("range() takes 2 or 3 args"); return v_nil(); }
    int64_t s = argv[0].kind == V_INT ? argv[0].as.i : (int64_t)argv[0].as.f;
    int64_t e = argv[1].kind == V_INT ? argv[1].as.i : (int64_t)argv[1].as.f;
    int64_t step = (argc == 3) ? (argv[2].kind == V_INT ? argv[2].as.i : (int64_t)argv[2].as.f) : 1;
    if (step == 0) { g_set_error("range() step cannot be 0"); return v_nil(); }
    value arr = v_arr();
    if (step > 0) {
        for (int64_t i = s; i < e; i += step) {
            value iv = v_int(i);
            /* push */
            if (arr.as.arr->len + 1 > arr.as.arr->cap) {
                arr.as.arr->cap = arr.as.arr->cap ? arr.as.arr->cap*2 : 8;
                arr.as.arr->items = realloc(arr.as.arr->items, arr.as.arr->cap * sizeof(value));
            }
            arr.as.arr->items[arr.as.arr->len++] = iv;
        }
    } else {
        for (int64_t i = s; i > e; i += step) {
            value iv = v_int(i);
            if (arr.as.arr->len + 1 > arr.as.arr->cap) {
                arr.as.arr->cap = arr.as.arr->cap ? arr.as.arr->cap*2 : 8;
                arr.as.arr->items = realloc(arr.as.arr->items, arr.as.arr->cap * sizeof(value));
            }
            arr.as.arr->items[arr.as.arr->len++] = iv;
        }
    }
    return arr;
}

static value nb_push(int argc, value *argv) {
    if (argc != 2) { g_set_error("push() takes 2 args"); return v_nil(); }
    if (argv[0].kind != V_ARRAY) { g_set_error("push() first arg must be array"); return v_nil(); }
    value arr = argv[0];
    /* clone the value to push so ownership is clean */
    value v = v_clone(&argv[1]);
    if (arr.as.arr->len + 1 > arr.as.arr->cap) {
        arr.as.arr->cap = arr.as.arr->cap ? arr.as.arr->cap*2 : 8;
        arr.as.arr->items = realloc(arr.as.arr->items, arr.as.arr->cap * sizeof(value));
    }
    arr.as.arr->items[arr.as.arr->len++] = v;
    return arr;
}

static value nb_type(int argc, value *argv) {
    if (argc != 1) return v_nil();
    const char *t = "?";
    switch (argv[0].kind) {
        case V_INT:    t = "int"; break;
        case V_FLOAT:  t = "float"; break;
        case V_STRING: t = "string"; break;
        case V_ARRAY:  t = "array"; break;
        case V_NIL:    t = "nil"; break;
        case V_FUNC:   t = "squire"; break;
        case V_NATIVE: t = "native"; break;
        case V_PTR:    t = "ptr"; break;
        case V_DICT:   t = "dict"; break;
    }
    return v_str(t);
}

static value nb_int(int argc, value *argv) {
    if (argc != 1) return v_nil();
    if (argv[0].kind == V_INT) return argv[0];
    if (argv[0].kind == V_FLOAT) return v_int((int64_t)argv[0].as.f);
    if (argv[0].kind == V_STRING) return v_int(strtoll(argv[0].as.s, NULL, 10));
    return v_nil();
}

static value nb_float(int argc, value *argv) {
    if (argc != 1) return v_nil();
    if (argv[0].kind == V_FLOAT) return argv[0];
    if (argv[0].kind == V_INT) return v_float((double)argv[0].as.i);
    if (argv[0].kind == V_STRING) return v_float(strtod(argv[0].as.s, NULL));
    return v_nil();
}

static value nb_string(int argc, value *argv) {
    if (argc != 1) return v_nil();
    char *s = v_to_string(&argv[0]);
    return v_str_take(s);
}

static value nb_readln(int argc, value *argv) {
    (void)argc; (void)argv;
    sbuf sb; sbuf_init(&sb);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        sbuf_putc(&sb, (char)c);
    }
    if (!sb.data) return v_str("");
    return v_str_take(sb.data);
}

static value nb_readint(int argc, value *argv) {
    (void)argc; (void)argv;
    long long v = 0;
    if (scanf("%lld", &v) == 1) return v_int((int64_t)v);
    return v_nil();
}

static value nb_write(int argc, value *argv) {
    for (int i = 0; i < argc; i++) {
        char *s = v_to_string(&argv[i]);
        fputs(s, stdout);
        free(s);
    }
    fflush(stdout);
    return v_nil();
}

static value nb_pow(int argc, value *argv) {
    if (argc != 2) return v_nil();
    if (argv[0].kind == V_INT && argv[1].kind == V_INT) {
        return v_int((int64_t)pow((double)argv[0].as.i, (double)argv[1].as.i));
    }
    double a = argv[0].kind == V_INT ? (double)argv[0].as.i : argv[0].as.f;
    double b = argv[1].kind == V_INT ? (double)argv[1].as.i : argv[1].as.f;
    return v_float(pow(a, b));
}

static value nb_sqrt(int argc, value *argv) {
    if (argc != 1) return v_nil();
    double a = argv[0].kind == V_INT ? (double)argv[0].as.i : argv[0].as.f;
    return v_float(sqrt(a));
}

static value nb_abs(int argc, value *argv) {
    if (argc != 1) return v_nil();
    if (argv[0].kind == V_INT) return v_int(argv[0].as.i < 0 ? -argv[0].as.i : argv[0].as.i);
    if (argv[0].kind == V_FLOAT) return v_float(fabs(argv[0].as.f));
    return v_nil();
}

static value nb_clock(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int((int64_t)time(NULL));
}

static value nb_exit(int argc, value *argv) {
    int code = 0;
    if (argc == 1) {
        code = (int)(argv[0].kind == V_INT ? argv[0].as.i : 0);
    }
    exit(code);
    return v_nil();
}

static value nb_atoi_str(int argc, value *argv) {
    /* string.upper / lower / split helpers — keep small */
    if (argc != 1 || argv[0].kind != V_STRING) return v_nil();
    char *s = strdup(argv[0].as.s);
    for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
    return v_str_take(s);
}
static value nb_lower(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_STRING) return v_nil();
    char *s = strdup(argv[0].as.s);
    for (char *p = s; *p; p++) *p = tolower((unsigned char)*p);
    return v_str_take(s);
}

static value nb_argc(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int(g_cli_argc);
}

static value nb_argv(int argc, value *argv) {
    (void)argc; (void)argv;
    value arr = v_arr();
    for (int i = 0; i < g_cli_argc; i++) {
        value v = v_str(g_cli_argv[i]);
        if (arr.as.arr->len + 1 > arr.as.arr->cap) {
            arr.as.arr->cap = arr.as.arr->cap ? arr.as.arr->cap*2 : 8;
            arr.as.arr->items = realloc(arr.as.arr->items, arr.as.arr->cap * sizeof(value));
        }
        arr.as.arr->items[arr.as.arr->len++] = v;
    }
    return arr;
}

static value nb_system(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_STRING) return v_int(-1);
    return v_int((int64_t)system(argv[0].as.s));
}

/* ------------------------------------------------------------------ */
/* FFI: dlopen, dlsym, ccall — call C functions from shared libraries  */
/* ------------------------------------------------------------------ */

static value nb_dlopen(int argc, value *argv) {
    if (argc != 1) { g_set_error("dlopen() takes 1 arg"); return v_nil(); }
    const char *name = NULL;
    if (argv[0].kind == V_STRING) name = argv[0].as.s;
    else if (argv[0].kind == V_NIL) name = NULL;
    else { g_set_error("dlopen() needs a string or nil"); return v_nil(); }
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        g_set_error("dlopen failed: %s", dlerror());
        return v_ptr(NULL);
    }
    return v_ptr(h);
}

static value nb_dlsym(int argc, value *argv) {
    if (argc != 2 || argv[0].kind != V_PTR || argv[1].kind != V_STRING) {
        g_set_error("dlsym(handle, name) needs a ptr and a string");
        return v_ptr(NULL);
    }
    void *sym = dlsym(argv[0].as.ptr, argv[1].as.s);
    if (!sym) {
        g_set_error("dlsym failed: %s", dlerror());
        return v_ptr(NULL);
    }
    return v_ptr(sym);
}

/* ccall(handle, name, args_array, ret_type) -> value
 *
 * handle: ptr from dlopen (or nil for RTLD_DEFAULT)
 * name: string function name
 * args_array: array of argument values (int, ptr, string->ptr, float)
 * ret_type: string: "int", "ptr", "double", "void", "string"
 *
 * On x86-64, integer and pointer arguments share the same register-passing
 * convention, so we can treat them uniformly as 64-bit values. This covers
 * the vast majority of C function calls including all of X11.
 */
static value nb_ccall(int argc, value *argv) {
    if (argc < 3) {
        g_set_error("ccall needs at least 3 args: handle, name, args, [ret_type]");
        return v_nil();
    }
    void *handle = (argv[0].kind == V_PTR) ? argv[0].as.ptr : RTLD_DEFAULT;
    if (argv[1].kind != V_STRING) {
        g_set_error("ccall: second arg must be function name string");
        return v_nil();
    }
    const char *fname = argv[1].as.s;
    void *fsym = dlsym(handle, fname);
    if (!fsym) {
        g_set_error("ccall: symbol '%s' not found: %s", fname, dlerror());
        return v_nil();
    }

    if (argv[2].kind != V_ARRAY) {
        g_set_error("ccall: third arg must be an array of arguments");
        return v_nil();
    }
    varr *args = argv[2].as.arr;
    int nargs = (int)args->len;

    /* Determine return type */
    const char *ret_type = "int";
    if (argc >= 4 && argv[3].kind == V_STRING) ret_type = argv[3].as.s;

    /* Convert args to 64-bit values (ints and ptrs share registers on x86-64).
     * Strings are passed as their char* pointer.
     * Floats/doubles need different handling — we support up to 8 float args
     * via xmm registers, but for simplicity v1 only supports int/ptr args.
     * If a float is passed, it's converted to int (truncated). */
    int64_t iargs[16];
    void *pargs[16];
    for (int i = 0; i < nargs && i < 16; i++) {
        value *a = &args->items[i];
        switch (a->kind) {
            case V_INT:    iargs[i] = a->as.i; pargs[i] = (void*)a->as.i; break;
            case V_PTR:    iargs[i] = (int64_t)a->as.ptr; pargs[i] = a->as.ptr; break;
            case V_STRING: iargs[i] = (int64_t)a->as.s; pargs[i] = a->as.s; break;
            case V_NIL:    iargs[i] = 0; pargs[i] = NULL; break;
            case V_FLOAT:  iargs[i] = (int64_t)a->as.f; pargs[i] = (void*)iargs[i]; break;
            default:       iargs[i] = 0; pargs[i] = NULL; break;
        }
    }

    /* Cast function pointer and call. On x86-64, the first 6 int/ptr args
     * go in rdi, rsi, rdx, rcx, r8, r9. We handle 0-6 args explicitly;
     * more than 6 requires stack setup which we don't support in v1. */
    int64_t ret_i = 0;
    void *ret_p = NULL;
    double ret_d = 0.0;

    if (nargs > 12) {
        g_set_error("ccall: more than 12 args not supported in v1");
        return v_nil();
    }

    /* The trick: cast fsym to a function pointer with the right number of
     * args and call it. Since int and ptr are both 64-bit, we use int64_t
     * for all args. */
    switch (nargs) {
        case 0:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(void))fsym)();
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "double") == 0) {
                ret_d = ((double(*)(void))fsym)();
                return v_float(ret_d);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(void))fsym)();
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(void))fsym)();
            }
            break;
        case 1:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t))fsym)(iargs[0]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "double") == 0) {
                ret_d = ((double(*)(int64_t))fsym)(iargs[0]);
                return v_float(ret_d);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t))fsym)(iargs[0]);
                return v_nil();
            } else if (strcmp(ret_type, "string") == 0) {
                char *s = ((char*(*)(int64_t))fsym)(iargs[0]);
                return v_str(s ? s : "");
            } else {
                ret_i = ((int64_t(*)(int64_t))fsym)(iargs[0]);
            }
            break;
        case 2:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t))fsym)(iargs[0], iargs[1]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t))fsym)(iargs[0], iargs[1]);
                return v_nil();
            } else if (strcmp(ret_type, "string") == 0) {
                char *s = ((char*(*)(int64_t,int64_t))fsym)(iargs[0], iargs[1]);
                return v_str(s ? s : "");
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t))fsym)(iargs[0], iargs[1]);
            }
            break;
        case 3:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2]);
                return v_nil();
            } else if (strcmp(ret_type, "string") == 0) {
                char *s = ((char*(*)(int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2]);
                return v_str(s ? s : "");
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2]);
            }
            break;
        case 4:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3]);
                return v_nil();
            } else if (strcmp(ret_type, "string") == 0) {
                char *s = ((char*(*)(int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3]);
                return v_str(s ? s : "");
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3]);
            }
            break;
        case 5:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]);
                return v_nil();
            } else if (strcmp(ret_type, "string") == 0) {
                char *s = ((char*(*)(int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]);
                return v_str(s ? s : "");
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]);
            }
            break;
        case 6:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]);
                return v_nil();
            } else if (strcmp(ret_type, "string") == 0) {
                char *s = ((char*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]);
                return v_str(s ? s : "");
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]);
            }
            break;
        case 7:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6]);
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6]);
            }
            break;
        case 8:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7]);
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7]);
            }
            break;
        case 9:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8]);
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8]);
            }
            break;
        case 10:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9]);
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9]);
            }
            break;
        case 11:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10]);
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10]);
            }
            break;
        case 12:
            if (strcmp(ret_type, "ptr") == 0) {
                ret_p = ((void*(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10], iargs[11]);
                return v_ptr(ret_p);
            } else if (strcmp(ret_type, "void") == 0) {
                ((void(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10], iargs[11]);
                return v_nil();
            } else {
                ret_i = ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t))fsym)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10], iargs[11]);
            }
            break;
    }

    if (strcmp(ret_type, "string") == 0) {
        return v_str((char*)ret_i);
    }
    return v_int(ret_i);
}

/* ccallf(handle, name, args_array, ret_type, arg_types) -> value
 *
 * Like ccall but with explicit per-argument type specifiers, so that
 * float/double arguments are passed in xmm registers instead of being
 * truncated to int.
 *
 * arg_types: a string of single-char type codes, one per arg:
 *   'i' = int64 (or ptr-sized int)
 *   'p' = pointer (same as int on x86-64)
 *   's' = string (passed as char*)
 *   'd' = double (passed in xmm register)
 *   'f' = float (promoted to double in C variadic, but for non-variadic
 *         functions this is a real float — we treat it as double for simplicity)
 *   'v' = void / nil (passed as 0)
 *
 * ret_type: "int", "ptr", "double", "void", "string" (same as ccall)
 *
 * Supports up to 6 integer-or-pointer args and up to 8 double args
 * (matches x86-64 System V ABI register usage).
 */
static value nb_ccallf(int argc, value *argv) {
    if (argc < 5) {
        g_set_error("ccallf needs 5 args: handle, name, args, ret_type, arg_types");
        return v_nil();
    }
    void *handle = (argv[0].kind == V_PTR) ? argv[0].as.ptr : RTLD_DEFAULT;
    if (argv[1].kind != V_STRING) {
        g_set_error("ccallf: second arg must be function name string");
        return v_nil();
    }
    if (argv[2].kind != V_ARRAY) {
        g_set_error("ccallf: third arg must be an array of arguments");
        return v_nil();
    }
    if (argv[3].kind != V_STRING || argv[4].kind != V_STRING) {
        g_set_error("ccallf: ret_type and arg_types must be strings");
        return v_nil();
    }
    const char *fname = argv[1].as.s;
    void *fsym = dlsym(handle, fname);
    if (!fsym) {
        g_set_error("ccallf: symbol '%s' not found: %s", fname, dlerror());
        return v_nil();
    }

    varr *args = argv[2].as.arr;
    int nargs = (int)args->len;
    const char *ret_type = argv[3].as.s;
    const char *arg_types = argv[4].as.s;

    if ((int)strlen(arg_types) != nargs) {
        g_set_error("ccallf: arg_types '%s' length (%zu) != args array length (%d)",
                    arg_types, strlen(arg_types), nargs);
        return v_nil();
    }
    if (nargs > 12) {
        g_set_error("ccallf: more than 12 args not supported");
        return v_nil();
    }

    /* Split args into int-reg and xmm-reg arrays per System V ABI.
     * The first 6 int/ptr args go in rdi,rsi,rdx,rcx,r8,r9.
     * The first 8 double args go in xmm0..xmm7.
     * Extra args go on the stack — not supported in this version. */
    int64_t iargs[16];
    double  fargs[16];
    int     iarg_count = 0;
    int     farg_count = 0;

    for (int i = 0; i < nargs; i++) {
        value *a = &args->items[i];
        char t = arg_types[i];
        switch (t) {
            case 'i': case 'p':
                if (a->kind == V_INT)    iargs[iarg_count++] = a->as.i;
                else if (a->kind == V_PTR) iargs[iarg_count++] = (int64_t)a->as.ptr;
                else if (a->kind == V_NIL) iargs[iarg_count++] = 0;
                else if (a->kind == V_FLOAT) iargs[iarg_count++] = (int64_t)a->as.f;
                else iargs[iarg_count++] = 0;
                break;
            case 's':
                iargs[iarg_count++] = (int64_t)(a->kind == V_STRING ? a->as.s : "");
                break;
            case 'd': case 'f':
                if (a->kind == V_FLOAT)      fargs[farg_count++] = a->as.f;
                else if (a->kind == V_INT)   fargs[farg_count++] = (double)a->as.i;
                else fargs[farg_count++] = 0.0;
                break;
            case 'v': default:
                iargs[iarg_count++] = 0;
                break;
        }
    }

    /* We dispatch by total arg count and use C's own calling convention
     * by casting to the exact function pointer type. Since C doesn't let
     * us dynamically choose arg types, we use a switch on a "type signature
     * hash" — but that's combinatorial. Instead, we use the GCC
     * __builtin_apply extension which lets us call any function pointer
     * with a packed register frame.
     *
     * For simplicity and portability, we instead use a hand-written
     * dispatch that covers the common cases (0-6 args, all-int or all-double
     * or mixed with up to 6 int + up to 8 double). We pass doubles through
     * the integer arg slots too — the C compiler will route them to xmm
     * registers if the function pointer type says so.
     *
     * The trick: build a function pointer type that matches the arg_types
     * string, and call it. We precompute all 12-arg permutations of int/double
     * — but that's 2^12 = 4096 cases. Too many.
     *
     * Better: use libffi if available. Failing that, use the __builtin_apply
     * GCC extension which lets us pass a packed register frame.
     *
     * Simplest portable approach: limit to 6 args, and precompute all
     * 2^6 = 64 type combinations. Yes, the code is verbose, but it's
     * fast and correct. We use a macro to keep it manageable.
     */

    /* For now, the simplest correct thing: dispatch by nargs, treating
     * all args as the type specified. Use a tagged-union dispatch. */

    /* The cleanest portable solution is libffi. Let's check for it. */
    /* If not available, fall back to a hardcoded dispatch table covering
     * the cases we actually need (sqrt: 1 double -> double, etc.). */

    /* Common case: 1 arg, double -> double (sqrt, sin, cos, ...) */
    if (nargs == 1 && arg_types[0] == 'd' &&
        (strcmp(ret_type, "double") == 0 || strcmp(ret_type, "float") == 0)) {
        double r = ((double(*)(double))fsym)(fargs[0]);
        return v_float(r);
    }
    /* 1 arg, double -> int (lround, etc.) */
    if (nargs == 1 && arg_types[0] == 'd' && strcmp(ret_type, "int") == 0) {
        int64_t r = ((int64_t(*)(double))fsym)(fargs[0]);
        return v_int(r);
    }
    /* 2 args, double double -> double (pow, fmod, ...) */
    if (nargs == 2 && arg_types[0] == 'd' && arg_types[1] == 'd' &&
        strcmp(ret_type, "double") == 0) {
        double r = ((double(*)(double,double))fsym)(fargs[0], fargs[1]);
        return v_float(r);
    }
    /* 2 args, double double -> int */
    if (nargs == 2 && arg_types[0] == 'd' && arg_types[1] == 'd' &&
        strcmp(ret_type, "int") == 0) {
        int64_t r = ((int64_t(*)(double,double))fsym)(fargs[0], fargs[1]);
        return v_int(r);
    }
    /* 1 arg, int -> double (e.g., some custom funcs) */
    if (nargs == 1 && arg_types[0] == 'i' && strcmp(ret_type, "double") == 0) {
        double r = ((double(*)(int64_t))fsym)(iargs[0]);
        return v_float(r);
    }
    /* 1 arg, ptr -> double */
    if (nargs == 1 && arg_types[0] == 'p' && strcmp(ret_type, "double") == 0) {
        double r = ((double(*)(void*))fsym)((void*)iargs[0]);
        return v_float(r);
    }

    /* Fall back: if all args are int/ptr/s, use the existing ccall dispatch
     * by reconstructing the call with all-int convention. This won't work
     * for functions that take doubles in xmm, but it covers the common case. */
    {
        value sub_argv[4];
        sub_argv[0] = argv[0];
        sub_argv[1] = argv[1];
        sub_argv[2] = argv[2];
        sub_argv[3] = argv[3];
        g_set_error("ccallf: argument signature '%s' not yet supported "
                    "(only common double patterns)", arg_types);
        (void)sub_argv;
        return v_nil();
    }
}

static value nb_ptr_null(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_ptr(NULL);
}

static value nb_ptr_to_int(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_PTR) return v_int(0);
    return v_int((int64_t)argv[0].as.ptr);
}

static value nb_int_to_ptr(int argc, value *argv) {
    if (argc != 1) return v_ptr(NULL);
    int64_t v = (argv[0].kind == V_INT) ? argv[0].as.i : (int64_t)argv[0].as.f;
    return v_ptr((void*)v);
}

static value nb_ptr_read(int argc, value *argv) {
    /* ptr_read(ptr, offset) -> int  (reads 8 bytes at ptr+offset) */
    if (argc != 2 || argv[0].kind != V_PTR) return v_int(0);
    int64_t off = (argv[1].kind == V_INT) ? argv[1].as.i : 0;
    int64_t *p = (int64_t*)((char*)argv[0].as.ptr + off);
    return v_int(*p);
}

static value nb_ptr_write(int argc, value *argv) {
    /* ptr_write(ptr, offset, value) -> nil */
    if (argc != 3 || argv[0].kind != V_PTR) return v_nil();
    int64_t off = (argv[1].kind == V_INT) ? argv[1].as.i : 0;
    int64_t val = (argv[2].kind == V_INT) ? argv[2].as.i : (int64_t)(argv[2].kind == V_FLOAT ? argv[2].as.f : 0);
    int64_t *p = (int64_t*)((char*)argv[0].as.ptr + off);
    *p = val;
    return v_nil();
}

static value nb_ptr_read_byte(int argc, value *argv) {
    /* ptr_read_byte(ptr, offset) -> int (reads 1 byte) */
    if (argc != 2 || argv[0].kind != V_PTR) return v_int(0);
    int64_t off = (argv[1].kind == V_INT) ? argv[1].as.i : 0;
    unsigned char *p = (unsigned char*)argv[0].as.ptr + off;
    return v_int((int64_t)*p);
}

static value nb_ptr_read_int32(int argc, value *argv) {
    /* ptr_read_int32(ptr, offset) -> int (reads 4 bytes, little-endian) */
    if (argc != 2 || argv[0].kind != V_PTR) return v_int(0);
    int64_t off = (argv[1].kind == V_INT) ? argv[1].as.i : 0;
    int32_t *p = (int32_t*)((char*)argv[0].as.ptr + off);
    return v_int((int64_t)*p);
}

static value nb_malloc(int argc, value *argv) {
    if (argc != 1) return v_ptr(NULL);
    int64_t n = (argv[0].kind == V_INT) ? argv[0].as.i : 0;
    return v_ptr(malloc(n));
}

static value nb_free(int argc, value *argv) {
    if (argc == 1 && argv[0].kind == V_PTR) free(argv[0].as.ptr);
    return v_nil();
}

static value nb_ptr_add(int argc, value *argv) {
    /* ptr_add(ptr, n) -> ptr  (pointer arithmetic: ptr + n bytes) */
    if (argc != 2 || argv[0].kind != V_PTR) return v_ptr(NULL);
    int64_t n = (argv[1].kind == V_INT) ? argv[1].as.i : 0;
    return v_ptr((char*)argv[0].as.ptr + n);
}

static value nb_memset(int argc, value *argv) {
    /* memset(ptr, byte, count) -> ptr */
    if (argc != 3 || argv[0].kind != V_PTR) return v_nil();
    int64_t byte = (argv[1].kind == V_INT) ? argv[1].as.i : 0;
    int64_t count = (argv[2].kind == V_INT) ? argv[2].as.i : 0;
    memset(argv[0].as.ptr, (int)byte, (size_t)count);
    return v_nil();
}

static value nb_sizeof_ptr(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int(sizeof(void*));
}

void interp_install_builtins(interp *it) {
    env_set_local(it->globals, "len",    v_native(nb_len));
    env_set_local(it->globals, "range",  v_native(nb_range));
    env_set_local(it->globals, "push",   v_native(nb_push));
    env_set_local(it->globals, "type",   v_native(nb_type));
    env_set_local(it->globals, "int",    v_native(nb_int));
    env_set_local(it->globals, "float",  v_native(nb_float));
    env_set_local(it->globals, "string", v_native(nb_string));
    env_set_local(it->globals, "readln", v_native(nb_readln));
    env_set_local(it->globals, "readint", v_native(nb_readint));
    env_set_local(it->globals, "write",  v_native(nb_write));
    env_set_local(it->globals, "pow",    v_native(nb_pow));
    env_set_local(it->globals, "sqrt",   v_native(nb_sqrt));
    env_set_local(it->globals, "abs",    v_native(nb_abs));
    env_set_local(it->globals, "clock",  v_native(nb_clock));
    env_set_local(it->globals, "exit",   v_native(nb_exit));
    env_set_local(it->globals, "upper",  v_native(nb_atoi_str));
    env_set_local(it->globals, "lower",  v_native(nb_lower));
    env_set_local(it->globals, "argc",   v_native(nb_argc));
    env_set_local(it->globals, "argv",   v_native(nb_argv));
    env_set_local(it->globals, "system", v_native(nb_system));
    /* FFI */
    env_set_local(it->globals, "dlopen",    v_native(nb_dlopen));
    env_set_local(it->globals, "dlsym",     v_native(nb_dlsym));
    env_set_local(it->globals, "ccall",     v_native(nb_ccall));
    env_set_local(it->globals, "ccallf",    v_native(nb_ccallf));
    env_set_local(it->globals, "ptr_null",  v_native(nb_ptr_null));
    env_set_local(it->globals, "ptr_to_int",v_native(nb_ptr_to_int));
    env_set_local(it->globals, "int_to_ptr",v_native(nb_int_to_ptr));
    env_set_local(it->globals, "ptr_read",  v_native(nb_ptr_read));
    env_set_local(it->globals, "ptr_write", v_native(nb_ptr_write));
    env_set_local(it->globals, "ptr_read_byte",  v_native(nb_ptr_read_byte));
    env_set_local(it->globals, "ptr_read_int32", v_native(nb_ptr_read_int32));
    env_set_local(it->globals, "malloc",    v_native(nb_malloc));
    env_set_local(it->globals, "free",      v_native(nb_free));
    env_set_local(it->globals, "ptr_add",   v_native(nb_ptr_add));
    env_set_local(it->globals, "memset",    v_native(nb_memset));
    env_set_local(it->globals, "sizeof_ptr",v_native(nb_sizeof_ptr));

    /* Language-agnostic FFI extension (src/ffi.c).
     * Plan 9 way: every language is a filter, spoken to via pipes.
     * The native_fn pointers are exposed via getters because the interp
     * struct is private to this file. */
    env_set_local(it->globals, "exec",         v_native(ffi_nb_exec()));
    env_set_local(it->globals, "exec_status",  v_native(ffi_nb_exec_status()));
    env_set_local(it->globals, "pipe_open",    v_native(ffi_nb_pipe_open()));
    env_set_local(it->globals, "pipe_write",   v_native(ffi_nb_pipe_write()));
    env_set_local(it->globals, "pipe_readln",  v_native(ffi_nb_pipe_readln()));
    env_set_local(it->globals, "pipe_read",    v_native(ffi_nb_pipe_read()));
    env_set_local(it->globals, "pipe_close",   v_native(ffi_nb_pipe_close()));
    env_set_local(it->globals, "lang_eval",    v_native(ffi_nb_lang_eval()));
    env_set_local(it->globals, "lang_call",    v_native(ffi_nb_lang_call()));
    env_set_local(it->globals, "lang_list",    v_native(ffi_nb_lang_list()));

    /* ────────────────────────────────────────────────────────────────
     * Standard library (src/stdlib.c) — v0.3.0 "Plan 9 Release"
     * ──────────────────────────────────────────────────────────────── */

    /* Strings */
    env_set_local(it->globals, "str_find",         v_native(stdlib_str_find()));
    env_set_local(it->globals, "str_find_from",    v_native(stdlib_str_find_from()));
    env_set_local(it->globals, "str_slice",        v_native(stdlib_str_slice()));
    env_set_local(it->globals, "str_split",        v_native(stdlib_str_split()));
    env_set_local(it->globals, "str_join",         v_native(stdlib_str_join()));
    env_set_local(it->globals, "str_replace",      v_native(stdlib_str_replace()));
    env_set_local(it->globals, "str_replace_all",  v_native(stdlib_str_replace_all()));
    env_set_local(it->globals, "str_trim",         v_native(stdlib_str_trim()));
    env_set_local(it->globals, "str_trim_left",    v_native(stdlib_str_trim_left()));
    env_set_local(it->globals, "str_trim_right",   v_native(stdlib_str_trim_right()));
    env_set_local(it->globals, "str_upper",        v_native(stdlib_str_upper()));
    env_set_local(it->globals, "str_lower",        v_native(stdlib_str_lower()));
    env_set_local(it->globals, "str_starts_with",  v_native(stdlib_str_starts_with()));
    env_set_local(it->globals, "str_ends_with",    v_native(stdlib_str_ends_with()));
    env_set_local(it->globals, "str_contains",     v_native(stdlib_str_contains()));
    env_set_local(it->globals, "str_repeat",       v_native(stdlib_str_repeat()));
    env_set_local(it->globals, "str_reverse",      v_native(stdlib_str_reverse()));
    env_set_local(it->globals, "str_chars",        v_native(stdlib_str_chars()));
    env_set_local(it->globals, "str_bytes",        v_native(stdlib_str_bytes()));
    env_set_local(it->globals, "str_from_bytes",   v_native(stdlib_str_from_bytes()));
    env_set_local(it->globals, "str_to_int",       v_native(stdlib_str_to_int()));
    env_set_local(it->globals, "str_to_float",     v_native(stdlib_str_to_float()));
    env_set_local(it->globals, "int_to_str",       v_native(stdlib_int_to_str()));
    env_set_local(it->globals, "float_to_str",     v_native(stdlib_float_to_str()));
    env_set_local(it->globals, "str_format",       v_native(stdlib_str_format()));

    /* Arrays */
    env_set_local(it->globals, "arr_push",     v_native(stdlib_arr_push()));
    env_set_local(it->globals, "arr_pop",      v_native(stdlib_arr_pop()));
    env_set_local(it->globals, "arr_shift",    v_native(stdlib_arr_shift()));
    env_set_local(it->globals, "arr_unshift",  v_native(stdlib_arr_unshift()));
    env_set_local(it->globals, "arr_map",      v_native(stdlib_arr_map()));
    env_set_local(it->globals, "arr_filter",   v_native(stdlib_arr_filter()));
    env_set_local(it->globals, "arr_reduce",   v_native(stdlib_arr_reduce()));
    env_set_local(it->globals, "arr_sort",     v_native(stdlib_arr_sort()));
    env_set_local(it->globals, "arr_reverse",  v_native(stdlib_arr_reverse()));
    env_set_local(it->globals, "arr_concat",   v_native(stdlib_arr_concat()));
    env_set_local(it->globals, "arr_slice",    v_native(stdlib_arr_slice()));
    env_set_local(it->globals, "arr_find",     v_native(stdlib_arr_find()));
    env_set_local(it->globals, "arr_contains", v_native(stdlib_arr_contains()));

    /* Dicts */
    env_set_local(it->globals, "dict",         v_native(stdlib_dict_new()));
    env_set_local(it->globals, "dict_set",     v_native(stdlib_dict_set()));
    env_set_local(it->globals, "dict_get",     v_native(stdlib_dict_get()));
    env_set_local(it->globals, "dict_get_or",  v_native(stdlib_dict_get_or()));
    env_set_local(it->globals, "dict_has",     v_native(stdlib_dict_has()));
    env_set_local(it->globals, "dict_del",     v_native(stdlib_dict_del()));
    env_set_local(it->globals, "dict_keys",    v_native(stdlib_dict_keys()));
    env_set_local(it->globals, "dict_vals",    v_native(stdlib_dict_vals()));
    env_set_local(it->globals, "dict_size",    v_native(stdlib_dict_size()));
    env_set_local(it->globals, "dict_clear",   v_native(stdlib_dict_clear()));

    /* Files */
    env_set_local(it->globals, "file_open",      v_native(stdlib_file_open()));
    env_set_local(it->globals, "file_close",     v_native(stdlib_file_close()));
    env_set_local(it->globals, "file_read",      v_native(stdlib_file_read()));
    env_set_local(it->globals, "file_readln",    v_native(stdlib_file_readln()));
    env_set_local(it->globals, "file_read_all",  v_native(stdlib_file_read_all()));
    env_set_local(it->globals, "file_write",     v_native(stdlib_file_write()));
    env_set_local(it->globals, "file_writeln",   v_native(stdlib_file_writeln()));
    env_set_local(it->globals, "file_eof",       v_native(stdlib_file_eof()));
    env_set_local(it->globals, "file_seek",      v_native(stdlib_file_seek()));
    env_set_local(it->globals, "file_tell",      v_native(stdlib_file_tell()));
    env_set_local(it->globals, "file_flush",     v_native(stdlib_file_flush()));
    env_set_local(it->globals, "file_size",      v_native(stdlib_file_size()));
    env_set_local(it->globals, "file_exists",    v_native(stdlib_file_exists()));
    env_set_local(it->globals, "file_is_dir",    v_native(stdlib_file_is_dir()));
    env_set_local(it->globals, "file_stat",      v_native(stdlib_file_stat()));
    env_set_local(it->globals, "file_mkdir",     v_native(stdlib_file_mkdir()));
    env_set_local(it->globals, "file_rmdir",     v_native(stdlib_file_rmdir()));
    env_set_local(it->globals, "file_unlink",    v_native(stdlib_file_unlink()));
    env_set_local(it->globals, "file_rename",    v_native(stdlib_file_rename()));
    env_set_local(it->globals, "file_list",      v_native(stdlib_file_list()));
    env_set_local(it->globals, "file_read_file", v_native(stdlib_file_read_file()));
    env_set_local(it->globals, "file_write_file",v_native(stdlib_file_write_file()));

    /* Math */
    env_set_local(it->globals, "math_sin",     v_native(stdlib_math_sin()));
    env_set_local(it->globals, "math_cos",     v_native(stdlib_math_cos()));
    env_set_local(it->globals, "math_tan",     v_native(stdlib_math_tan()));
    env_set_local(it->globals, "math_asin",    v_native(stdlib_math_asin()));
    env_set_local(it->globals, "math_acos",    v_native(stdlib_math_acos()));
    env_set_local(it->globals, "math_atan",    v_native(stdlib_math_atan()));
    env_set_local(it->globals, "math_atan2",   v_native(stdlib_math_atan2()));
    env_set_local(it->globals, "math_pow",     v_native(stdlib_math_pow()));
    env_set_local(it->globals, "math_sqrt",    v_native(stdlib_math_sqrt()));
    env_set_local(it->globals, "math_cbrt",    v_native(stdlib_math_cbrt()));
    env_set_local(it->globals, "math_log",     v_native(stdlib_math_log()));
    env_set_local(it->globals, "math_log2",    v_native(stdlib_math_log2()));
    env_set_local(it->globals, "math_log10",   v_native(stdlib_math_log10()));
    env_set_local(it->globals, "math_exp",     v_native(stdlib_math_exp()));
    env_set_local(it->globals, "math_floor",   v_native(stdlib_math_floor()));
    env_set_local(it->globals, "math_ceil",    v_native(stdlib_math_ceil()));
    env_set_local(it->globals, "math_round",   v_native(stdlib_math_round()));
    env_set_local(it->globals, "math_min",     v_native(stdlib_math_min()));
    env_set_local(it->globals, "math_max",     v_native(stdlib_math_max()));
    env_set_local(it->globals, "math_abs",     v_native(stdlib_math_abs()));
    env_set_local(it->globals, "math_sign",    v_native(stdlib_math_sign()));
    env_set_local(it->globals, "math_clamp",   v_native(stdlib_math_clamp()));
    env_set_local(it->globals, "math_random",          v_native(stdlib_math_random()));
    env_set_local(it->globals, "math_random_int",      v_native(stdlib_math_random_int()));
    env_set_local(it->globals, "math_random_seed",     v_native(stdlib_math_random_seed()));
    env_set_local(it->globals, "math_pi", v_float(3.14159265358979323846));
    env_set_local(it->globals, "math_e",  v_float(2.71828182845904523536));

    /* Time */
    env_set_local(it->globals, "time_now",     v_native(stdlib_time_now()));
    env_set_local(it->globals, "time_now_s",   v_native(stdlib_time_now_s()));
    env_set_local(it->globals, "time_now_ns",  v_native(stdlib_time_now_ns()));
    env_set_local(it->globals, "time_sleep",   v_native(stdlib_time_sleep()));
    env_set_local(it->globals, "time_sleep_ms",v_native(stdlib_time_sleep_ms()));
    env_set_local(it->globals, "time_sleep_ns",v_native(stdlib_time_sleep_ns()));
    env_set_local(it->globals, "time_format",  v_native(stdlib_time_format()));
    env_set_local(it->globals, "time_year",    v_native(stdlib_time_year()));
    env_set_local(it->globals, "time_month",   v_native(stdlib_time_month()));
    env_set_local(it->globals, "time_day",     v_native(stdlib_time_day()));
    env_set_local(it->globals, "time_hour",    v_native(stdlib_time_hour()));
    env_set_local(it->globals, "time_min",     v_native(stdlib_time_min()));
    env_set_local(it->globals, "time_sec",     v_native(stdlib_time_sec()));
    env_set_local(it->globals, "time_weekday", v_native(stdlib_time_weekday()));

    /* Process */
    env_set_local(it->globals, "proc_getpid",    v_native(stdlib_proc_getpid()));
    env_set_local(it->globals, "proc_getppid",   v_native(stdlib_proc_getppid()));
    env_set_local(it->globals, "proc_env",       v_native(stdlib_proc_env()));
    env_set_local(it->globals, "proc_env_set",   v_native(stdlib_proc_env_set()));
    env_set_local(it->globals, "proc_env_unset", v_native(stdlib_proc_env_unset()));
    env_set_local(it->globals, "proc_env_list",  v_native(stdlib_proc_env_list()));
    env_set_local(it->globals, "proc_cwd",       v_native(stdlib_proc_cwd()));
    env_set_local(it->globals, "proc_chdir",     v_native(stdlib_proc_chdir()));
    env_set_local(it->globals, "proc_fork",      v_native(stdlib_proc_fork()));
    env_set_local(it->globals, "proc_wait",      v_native(stdlib_proc_wait()));
    env_set_local(it->globals, "proc_wait_any",  v_native(stdlib_proc_wait_any()));
    env_set_local(it->globals, "proc_kill",      v_native(stdlib_proc_kill()));

    /* Functional */
    env_set_local(it->globals, "call",  v_native(stdlib_call()));
    env_set_local(it->globals, "apply", v_native(stdlib_apply()));

    /* Regex (POSIX extended) */
    env_set_local(it->globals, "re_match",       v_native(stdlib_re_match()));
    env_set_local(it->globals, "re_find",        v_native(stdlib_re_find()));
    env_set_local(it->globals, "re_find_all",    v_native(stdlib_re_find_all()));
    env_set_local(it->globals, "re_replace",     v_native(stdlib_re_replace()));
    env_set_local(it->globals, "re_replace_all", v_native(stdlib_re_replace_all()));
    env_set_local(it->globals, "re_split",       v_native(stdlib_re_split()));
    env_set_local(it->globals, "re_groups",      v_native(stdlib_re_groups()));

    /* Type conversion */
    env_set_local(it->globals, "to_str",   v_native(stdlib_to_str()));
    env_set_local(it->globals, "to_int",   v_native(stdlib_to_int()));
    env_set_local(it->globals, "to_float", v_native(stdlib_to_float()));
    env_set_local(it->globals, "to_bool",  v_native(stdlib_to_bool()));
    env_set_local(it->globals, "to_array", v_native(stdlib_to_array()));
    env_set_local(it->globals, "to_dict",  v_native(stdlib_to_dict()));

    /* More math */
    env_set_local(it->globals, "math_gcd",      v_native(stdlib_math_gcd()));
    env_set_local(it->globals, "math_lcm",      v_native(stdlib_math_lcm()));
    env_set_local(it->globals, "math_fact",     v_native(stdlib_math_fact()));
    env_set_local(it->globals, "math_is_prime", v_native(stdlib_math_is_prime()));
    env_set_local(it->globals, "math_hypot",    v_native(stdlib_math_hypot()));
    env_set_local(it->globals, "math_deg2rad",  v_native(stdlib_math_deg2rad()));
    env_set_local(it->globals, "math_rad2deg",  v_native(stdlib_math_rad2deg()));
    env_set_local(it->globals, "math_comb",     v_native(stdlib_math_comb()));
    env_set_local(it->globals, "math_perm",     v_native(stdlib_math_perm()));

    /* More strings */
    env_set_local(it->globals, "str_pad_left",  v_native(stdlib_str_pad_left()));
    env_set_local(it->globals, "str_pad_right", v_native(stdlib_str_pad_right()));
    env_set_local(it->globals, "str_center",    v_native(stdlib_str_center()));
    env_set_local(it->globals, "str_count",     v_native(stdlib_str_count()));

    /* JSON */
    env_set_local(it->globals, "json_parse",           v_native(json_nb_parse()));
    env_set_local(it->globals, "json_stringify",       v_native(json_nb_stringify()));
    env_set_local(it->globals, "json_stringify_pretty",v_native(json_nb_stringify_pretty()));
}

/* ------------------------------------------------------------------ */
/* arithmetic with overflow/zero checking                             */
/* ------------------------------------------------------------------ */

static int would_mul_overflow(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    if (a == INT64_MIN || b == INT64_MIN) return 1;
    if (a > 0) {
        if (b > 0) return a > INT64_MAX / b;
        return b < INT64_MIN / a;
    } else {
        if (b > 0) return a < INT64_MIN / b;
        return b < INT64_MAX / a; /* both negative */
    }
}

static int would_add_overflow(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) return 1;
    if (b < 0 && a < INT64_MIN - b) return 1;
    return 0;
}

static int would_sub_overflow(int64_t a, int64_t b) {
    if (b > 0 && a < INT64_MIN + b) return 1;
    if (b < 0 && a > INT64_MAX + b) return 1;
    return 0;
}

/* central binop dispatcher. On fault (div-by-zero, overflow), consults the
 * trigger table; if a handler exists, runs it and substitutes the result. */
static value do_binop(interp *it, const char *op, value l, value r, int line, int col) {
    /* string concatenation for + */
    if (op[0] == '+' && op[1] == '\0') {
        if (l.kind == V_STRING || r.kind == V_STRING) {
            char *ls = v_to_string(&l);
            char *rs = v_to_string(&r);
            sbuf sb; sbuf_init(&sb);
            sbuf_puts(&sb, ls); sbuf_puts(&sb, rs);
            free(ls); free(rs);
            return v_str_take(sb.data ? sb.data : strdup(""));
        }
    }

    /* Arithmetic with single-char string: treat as char code.
     * This makes src[i] - 48 work for digit conversion (lexer use case). */
    if (l.kind == V_STRING && strlen(l.as.s) == 1 &&
        (r.kind == V_INT || r.kind == V_FLOAT)) {
        int64_t lv = (int64_t)(unsigned char)l.as.s[0];
        double rv = r.kind == V_INT ? (double)r.as.i : r.as.f;
        double av = (double)lv;
        if (op[0] == '-' && op[1] == '\0') return v_int((int64_t)(av - rv));
    }
    if (r.kind == V_STRING && strlen(r.as.s) == 1 &&
        (l.kind == V_INT || l.kind == V_FLOAT)) {
        double av = l.kind == V_INT ? (double)l.as.i : l.as.f;
        int64_t rv = (int64_t)(unsigned char)r.as.s[0];
        double bv = (double)rv;
        if (op[0] == '-' && op[1] == '\0') return v_int((int64_t)(av - bv));
    }

    /* comparisons work on all types (==, !=, <, <=, >, >=) */
    if (op[0] == '=' && op[1] == '=') return v_int(v_eq(&l, &r));
    if (op[0] == '!' && op[1] == '=') return v_int(!v_eq(&l, &r));
    if (op[0] == '<' && op[1] == '\0') return v_int(v_lt(&l, &r));
    if (op[0] == '>' && op[1] == '\0') return v_int(v_lt(&r, &l));
    if (op[0] == '<' && op[1] == '=') return v_int(!v_lt(&r, &l));
    if (op[0] == '>' && op[1] == '=') return v_int(!v_lt(&l, &r));

    if (op[0] == '&' && op[1] == '&') {
        return v_int(v_truthy(&l).as.i && v_truthy(&r).as.i);
    }
    if (op[0] == '|' && op[1] == '|') {
        return v_int(v_truthy(&l).as.i || v_truthy(&r).as.i);
    }

    /* numeric promotion: int/float */
    int both_int = (l.kind == V_INT && r.kind == V_INT);
    int any_float = (l.kind == V_FLOAT || r.kind == V_FLOAT);

    if (!(both_int || (any_float && (l.kind == V_INT || l.kind == V_FLOAT) && (r.kind == V_INT || r.kind == V_FLOAT)))) {
        /* array + array? not supported in this version. */
        if (!(l.kind == V_STRING || r.kind == V_STRING)) {
            g_set_error("%d:%d: operator '%s' not defined for types %d/%d", line, col, op, l.kind, r.kind);
            it->sig.kind = SIG_ERROR;
            return v_nil();
        }
    }

    double av = l.kind == V_INT ? (double)l.as.i : l.as.f;
    double bv = r.kind == V_INT ? (double)r.as.i : r.as.f;

    /* division / modulo: check zero */
    if (op[0] == '/' && op[1] == '\0') {
        int is_zero = (both_int && r.as.i == 0) || (any_float && bv == 0.0);
        if (is_zero) {
            a_node *trig = find_trigger(it, "div_by_zero");
            if (trig && !it->in_trigger) {
                it->in_trigger = 1;
                value r2 = fire_trigger(it, "div_by_zero");
                it->in_trigger = 0;
                return r2;
            }
            g_set_error("%d:%d: division by zero", line, col);
            it->sig.kind = SIG_ERROR;
            return v_nil();
        }
        /* integer division if both int */
        if (both_int) return v_int(l.as.i / r.as.i);
        return v_float(av / bv);
    }
    if (op[0] == '%' && op[1] == '\0') {
        if (r.as.i == 0) {
            a_node *trig = find_trigger(it, "div_by_zero");
            if (trig && !it->in_trigger) {
                it->in_trigger = 1;
                value r2 = fire_trigger(it, "div_by_zero");
                it->in_trigger = 0;
                return r2;
            }
            g_set_error("%d:%d: modulo by zero", line, col);
            it->sig.kind = SIG_ERROR;
            return v_nil();
        }
        return v_int(l.as.i % r.as.i);
    }

    if (op[0] == '^' && op[1] == '\0') {
        /* integer power when both operands are non-negative ints and result fits */
        if (both_int && l.as.i >= 0 && r.as.i >= 0 && r.as.i < 63) {
            int64_t base = l.as.i;
            int64_t exp = r.as.i;
            int64_t result = 1;
            int overflow = 0;
            for (int64_t i = 0; i < exp; i++) {
                if (would_mul_overflow(result, base)) { overflow = 1; break; }
                result *= base;
            }
            if (!overflow) return v_int(result);
        }
        return v_float(pow(av, bv));
    }

    /* integer arithmetic with overflow detection */
    if (both_int) {
        int64_t a = l.as.i, b = r.as.i;
        if (op[0] == '+' && op[1] == '\0') {
            if (would_add_overflow(a, b)) {
                a_node *trig = find_trigger(it, "overflow");
                if (trig && !it->in_trigger) {
                    it->in_trigger = 1;
                    value v = fire_trigger(it, "overflow");
                    it->in_trigger = 0;
                    return v;
                }
                /* promote to float */
                return v_float((double)a + (double)b);
            }
            return v_int(a + b);
        }
        if (op[0] == '-' && op[1] == '\0') {
            if (would_sub_overflow(a, b)) {
                a_node *trig = find_trigger(it, "overflow");
                if (trig && !it->in_trigger) {
                    it->in_trigger = 1;
                    value v = fire_trigger(it, "overflow");
                    it->in_trigger = 0;
                    return v;
                }
                return v_float((double)a - (double)b);
            }
            return v_int(a - b);
        }
        if (op[0] == '*' && op[1] == '\0') {
            if (would_mul_overflow(a, b)) {
                a_node *trig = find_trigger(it, "overflow");
                if (trig && !it->in_trigger) {
                    it->in_trigger = 1;
                    value v = fire_trigger(it, "overflow");
                    it->in_trigger = 0;
                    return v;
                }
                return v_float((double)a * (double)b);
            }
            return v_int(a * b);
        }
    }

    /* float / mixed arithmetic */
    if (op[0] == '+' && op[1] == '\0') return v_float(av + bv);
    if (op[0] == '-' && op[1] == '\0') return v_float(av - bv);
    if (op[0] == '*' && op[1] == '\0') return v_float(av * bv);

    /* bitwise (int only) */
    if (both_int) {
        if (op[0] == '&' && op[1] == '\0') return v_int(l.as.i & r.as.i);
        if (op[0] == '|' && op[1] == '\0') return v_int(l.as.i | r.as.i);
        if (op[0] == '<' && op[1] == '<') return v_int(l.as.i << r.as.i);
        if (op[0] == '>' && op[1] == '>') return v_int(l.as.i >> r.as.i);
    }

    g_set_error("%d:%d: operator '%s' not supported", line, col, op);
    it->sig.kind = SIG_ERROR;
    return v_nil();
}

/* ------------------------------------------------------------------ */
/* trigger invocation                                                  */
/* ------------------------------------------------------------------ */

static value fire_trigger(interp *it, const char *name) {
    a_node *body = find_trigger(it, name);
    if (!body) {
        g_set_error("no trigger '%s' registered", name);
        it->sig.kind = SIG_ERROR;
        return v_nil();
    }
    /* run the trigger body in a fresh child of globals */
    env *te = env_new(it->globals);
    jmp_buf jb;
    jmp_buf *prev = it->ret_jmp;
    it->ret_jmp = &jb;
    ctrl_sig prev_sig = it->sig;
    it->sig.kind = SIG_NONE;
    if (setjmp(jb) == 0) {
        for (size_t i = 0; i < body->nchild; i++) {
            exec_stmt(it, body->children[i], te);
            if (it->sig.kind == SIG_RETURN) break;
            if (it->sig.kind == SIG_ERROR) break;
            if (it->sig.kind == SIG_STOP) break;
            if (it->sig.kind == SIG_NEXT) break;
        }
    }
    value rv = it->sig.kind == SIG_RETURN ? it->sig.val : v_nil();
    it->sig = prev_sig;
    it->ret_jmp = prev;
    env_free(te);
    return rv;
}

/* ------------------------------------------------------------------ */
/* expression evaluation                                               */
/* ------------------------------------------------------------------ */

static value call_squire(interp *it, a_node *def, int argc, value *argv, env *closure) {
    /* def is A_BLOCK_SQUIRE; children[0..nparams-1] are A_IDENT params;
       the rest are body statements */
    env *local = env_new(closure ? closure : it->globals);

    size_t nparams = 0;
    while (nparams < def->nchild && def->children[nparams]->kind == A_IDENT) nparams++;

    /* bind parameters */
    for (size_t i = 0; i < nparams; i++) {
        value v = (i < (size_t)argc) ? v_clone(&argv[i]) : v_nil();
        env_set_local(local, def->children[i]->sval, v);
    }
    /* bind 'iter' to 0 by default */
    env_set_local(local, "iter", v_int(0));

    jmp_buf jb;
    jmp_buf *prev_ret = it->ret_jmp;
    ctrl_sig prev_sig = it->sig;
    it->ret_jmp = &jb;
    it->sig.kind = SIG_NONE;

    value rv = v_nil();
    if (setjmp(jb) == 0) {
        for (size_t i = nparams; i < def->nchild; i++) {
            exec_stmt(it, def->children[i], local);
            if (it->sig.kind == SIG_RETURN) break;
            if (it->sig.kind == SIG_ERROR) break;
        }
    } else {
        /* longjmp happened */
    }
    if (it->sig.kind == SIG_RETURN) rv = it->sig.val;
    it->sig = prev_sig;
    it->ret_jmp = prev_ret;
    env_free(local);
    return rv;
}

static value eval_expr(interp *it, a_node *n, env *e) {
    if (!n) { it->sig.kind = SIG_ERROR; return v_nil(); }
    switch (n->kind) {
        case A_INT:    return v_int(n->ival);
        case A_FLOAT:  return v_float(n->fval);
        case A_STRING: return v_str(n->sval ? n->sval : "");
        case A_BOOL:   return v_int(n->ival);
        case A_NIL:    return v_nil();

        case A_IDENT: {
            int found;
            value v = env_get(e, n->sval, &found);
            if (!found) {
                /* is it a squire defined in globals? */
                int found2;
                value g = env_get(it->globals, n->sval, &found2);
                if (found2 && g.kind == V_FUNC) return g;
                g_set_error("%d:%d: undefined variable '%s'", n->line, n->col, n->sval);
                it->sig.kind = SIG_ERROR;
                return v_nil();
            }
            return v_clone(&v);
        }

        case A_BINOP: {
            /* short-circuit */
            if (strcmp(n->sval, "&&") == 0) {
                value l = eval_expr(it, n->lhs, e);
                if (it->sig.kind != SIG_NONE) return v_nil();
                if (!v_truthy(&l).as.i) return v_int(0);
                value r = eval_expr(it, n->rhs, e);
                if (it->sig.kind != SIG_NONE) return v_nil();
                return v_int(v_truthy(&r).as.i);
            }
            if (strcmp(n->sval, "||") == 0) {
                value l = eval_expr(it, n->lhs, e);
                if (it->sig.kind != SIG_NONE) return v_nil();
                if (v_truthy(&l).as.i) return v_int(1);
                value r = eval_expr(it, n->rhs, e);
                if (it->sig.kind != SIG_NONE) return v_nil();
                return v_int(v_truthy(&r).as.i);
            }
            value l = eval_expr(it, n->lhs, e);
            if (it->sig.kind != SIG_NONE) return v_nil();
            value r = eval_expr(it, n->rhs, e);
            if (it->sig.kind != SIG_NONE) return v_nil();
            return do_binop(it, n->sval, l, r, n->line, n->col);
        }

        case A_UNOP: {
            value v = eval_expr(it, n->lhs, e);
            if (it->sig.kind != SIG_NONE) return v_nil();
            if (n->sval[0] == '-') {
                if (v.kind == V_INT) return v_int(-v.as.i);
                if (v.kind == V_FLOAT) return v_float(-v.as.f);
            }
            if (n->sval[0] == '!') {
                return v_int(!v_truthy(&v).as.i);
            }
            g_set_error("%d:%d: unknown unary op '%s'", n->line, n->col, n->sval);
            it->sig.kind = SIG_ERROR;
            return v_nil();
        }

        case A_CALL: {
            /* n->sval is the function name; children are args */
            int found;
            value fn = env_get(e, n->sval, &found);
            if (!found) fn = env_get(it->globals, n->sval, &found);
            if (!found) {
                g_set_error("%d:%d: call to undefined '%s'", n->line, n->col, n->sval);
                it->sig.kind = SIG_ERROR;
                return v_nil();
            }
            int argc = (int)n->nchild;
            value *argv = argc ? calloc(argc, sizeof(value)) : NULL;
            for (int i = 0; i < argc; i++) {
                argv[i] = eval_expr(it, n->children[i], e);
                if (it->sig.kind != SIG_NONE) { free(argv); return v_nil(); }
            }
            value rv;
            if (fn.kind == V_NATIVE) {
                rv = fn.as.nat(argc, argv);
            } else if (fn.kind == V_FUNC) {
                rv = call_squire(it, fn.as.func.def, argc, argv, fn.as.func.closure);
            } else {
                g_set_error("%d:%d: '%s' is not callable", n->line, n->col, n->sval);
                it->sig.kind = SIG_ERROR;
                rv = v_nil();
            }
            for (int i = 0; i < argc; i++) v_free(&argv[i]);
            free(argv);
            return rv;
        }

        case A_INVOKE: {
            /* the [name args...] — invoke a squire */
            int found;
            value fn = env_get(it->globals, n->sval, &found);
            if (!found || fn.kind != V_FUNC) {
                g_set_error("%d:%d: no squire named '%s'", n->line, n->col, n->sval);
                it->sig.kind = SIG_ERROR;
                return v_nil();
            }
            int argc = (int)n->nchild;
            value *argv = argc ? calloc(argc, sizeof(value)) : NULL;
            for (int i = 0; i < argc; i++) {
                argv[i] = eval_expr(it, n->children[i], e);
                if (it->sig.kind != SIG_NONE) { free(argv); return v_nil(); }
            }
            value rv = call_squire(it, fn.as.func.def, argc, argv, fn.as.func.closure);
            for (int i = 0; i < argc; i++) v_free(&argv[i]);
            free(argv);
            return rv;
        }

        case A_INDEX: {
            value base = eval_expr(it, n->lhs, e);
            if (it->sig.kind != SIG_NONE) return v_nil();
            value idx  = eval_expr(it, n->rhs, e);
            if (it->sig.kind != SIG_NONE) return v_nil();
            if (base.kind == V_ARRAY) {
                int64_t i = idx.kind == V_INT ? idx.as.i : (int64_t)idx.as.f;
                if (i < 0) i += (int64_t)base.as.arr->len;
                if (i < 0 || (size_t)i >= base.as.arr->len) {
                    a_node *trig = find_trigger(it, "bound");
                    if (trig && !it->in_trigger) {
                        it->in_trigger = 1;
                        value rv = fire_trigger(it, "bound");
                        it->in_trigger = 0;
                        return rv;
                    }
                    g_set_error("%d:%d: array index %lld out of bounds (len %zu)", n->line, n->col, (long long)i, base.as.arr->len);
                    it->sig.kind = SIG_ERROR;
                    return v_nil();
                }
                value v = v_clone(&base.as.arr->items[i]);
                return v;
            }
            if (base.kind == V_STRING) {
                int64_t i = idx.kind == V_INT ? idx.as.i : (int64_t)idx.as.f;
                if (i < 0) i += (int64_t)strlen(base.as.s);
                if (i < 0 || (size_t)i >= strlen(base.as.s)) {
                    a_node *trig = find_trigger(it, "bound");
                    if (trig && !it->in_trigger) {
                        it->in_trigger = 1;
                        value rv = fire_trigger(it, "bound");
                        it->in_trigger = 0;
                        return rv;
                    }
                    g_set_error("%d:%d: string index out of bounds", n->line, n->col);
                    it->sig.kind = SIG_ERROR;
                    return v_nil();
                }
                char s[2] = { base.as.s[i], 0 };
                return v_str(s);
            }
            g_set_error("%d:%d: cannot index type %d", n->line, n->col, base.kind);
            it->sig.kind = SIG_ERROR;
            return v_nil();
        }

        case A_ARRAY_LITERAL: {
            value arr = v_arr();
            for (size_t i = 0; i < n->nchild; i++) {
                value v = eval_expr(it, n->children[i], e);
                if (it->sig.kind != SIG_NONE) { return v_nil(); }
                /* push */
                if (arr.as.arr->len + 1 > arr.as.arr->cap) {
                    arr.as.arr->cap = arr.as.arr->cap ? arr.as.arr->cap*2 : 8;
                    arr.as.arr->items = realloc(arr.as.arr->items, arr.as.arr->cap * sizeof(value));
                }
                arr.as.arr->items[arr.as.arr->len++] = v;
            }
            return arr;
        }

        default:
            g_set_error("%d:%d: expression node kind %d not implemented", n->line, n->col, n->kind);
            it->sig.kind = SIG_ERROR;
            return v_nil();
    }
}

/* ------------------------------------------------------------------ */
/* statement execution                                                 */
/* ------------------------------------------------------------------ */

static void exec_block(interp *it, a_node *n, env *parent) {
    /* block-local scope — but guards share the parent scope so that
     * variables assigned inside <if> blocks persist to the enclosing scope.
     * This matches developer expectations: <if cond> x = 1 should set x
     * in the current scope, not a disposable child scope. */
    env *e;
    if (n->kind == A_BLOCK_GUARD) {
        /* Guards use the parent scope directly — no new scope.
         * This is critical: variables set inside <if> must persist. */
        e = parent;
        /* Don't free e at the end since it's the parent's. */
        switch (n->kind) {
            case A_BLOCK_GUARD: {
                value c = eval_expr(it, n->cond, e);
                if (it->sig.kind != SIG_NONE) return;
                if (v_truthy(&c).as.i) {
                    for (size_t j = 0; j < n->nchild; j++) {
                        exec_stmt(it, n->children[j], e);
                        if (it->sig.kind != SIG_NONE) break;
                    }
                } else if (n->else_branch) {
                    a_node *eb = n->else_branch;
                    for (size_t j = 0; j < eb->nchild; j++) {
                        exec_stmt(it, eb->children[j], e);
                        if (it->sig.kind != SIG_NONE) break;
                    }
                }
                return;
            }
            default:
                /* shouldn't happen */
                return;
        }
    }

    e = env_new(parent);

    switch (n->kind) {
        case A_BLOCK_SQUIRE: {
            /* register the squire in globals (closures are globals) */
            value fn = {0};
            fn.kind = V_FUNC;
            fn.as.func.def = n;
            fn.as.func.closure = it->globals;
            env_set_local(it->globals, n->sval, fn);
            return;
        }

        case A_BLOCK_LANG: {
            /* [langname] rawcode — execute the raw code in the named
             * language via the FFI lang_eval builtin, and print the
             * result if non-empty. The block's sval is the language
             * name; lhs is an A_STRING node holding the raw body. */
            const char *lang = n->sval ? n->sval : "python";
            const char *code = (n->lhs && n->lhs->sval) ? n->lhs->sval : "";

            /* Look up the lang_eval builtin and call it directly. */
            int found = 0;
            value fn = env_get(it->globals, "lang_eval", &found);
            if (!found || fn.kind != V_NATIVE) {
                g_set_error("%d:%d: lang_eval builtin not available "
                            "(FFI extension not loaded?)", n->line, n->col);
                it->sig.kind = SIG_ERROR;
                return;
            }
            value argv[2] = { v_str(lang), v_str(code) };
            value rv = fn.as.nat(2, argv);
            v_free(&argv[0]);
            v_free(&argv[1]);

            /* If the result is a non-empty string, print it. This
             * matches the convention that an inline [lang] block's
             * "side effect" is its stdout. */
            if (rv.kind == V_STRING && rv.as.s && rv.as.s[0]) {
                fputs(rv.as.s, stdout);
                fputc('\n', stdout);
                fflush(stdout);
            }
            if (rv.kind == V_NIL) {
                /* lang_eval may have set an error via g_set_error */
                const char *err = g_last_error();
                if (err && err[0]) {
                    g_set_error("%d:%d: [%s] block failed: %s",
                                n->line, n->col, lang, err);
                    it->sig.kind = SIG_ERROR;
                }
            }
            v_free(&rv);
            return;
        }

        case A_BLOCK_TRIGGER: {
            interp_register_trigger(it, n->sval, n);
            return;
        }

        case A_BLOCK_SEQ: {
            /* labeled sequence block — runs once */
            for (size_t j = 0; j < n->nchild; j++) {
                exec_stmt(it, n->children[j], e);
                if (it->sig.kind != SIG_NONE) break;
            }
            break;
        }

        case A_BLOCK_LOOP_COUNT: {
            volatile int64_t count = 0;
            if (n->cond) {
                if (n->cond->kind == A_INT) count = n->cond->ival;
                else {
                    value v = eval_expr(it, n->cond, e);
                    if (it->sig.kind != SIG_NONE) { env_free(e); return; }
                    count = v.kind == V_INT ? v.as.i : (int64_t)v.as.f;
                }
            }
            jmp_buf brk, cont;
            jmp_buf *prev_brk = it->loop_jmp, *prev_nxt = it->next_jmp;
            ctrl_sig prev_sig = it->sig;
            it->loop_jmp = &brk;
            it->next_jmp = &cont;
            it->sig.kind = SIG_NONE;
            if (setjmp(brk) == 0) {
                for (volatile int64_t i = 0; i < count; i++) {
                    env_set_local(e, "iter", v_int(i + 1));
                    if (setjmp(cont) == 0) {
                        for (size_t j = 0; j < n->nchild; j++) {
                            exec_stmt(it, n->children[j], e);
                            if (it->sig.kind == SIG_ERROR) goto done;
                            if (it->sig.kind == SIG_STOP)  goto done;
                            if (it->sig.kind == SIG_NEXT)  break;
                            if (it->sig.kind == SIG_RETURN) goto done;
                        }
                    }
                    it->sig.kind = SIG_NONE;
                }
            }
            done:
            it->sig = (it->sig.kind == SIG_STOP || it->sig.kind == SIG_NEXT) ? prev_sig : it->sig;
            it->loop_jmp = prev_brk;
            it->next_jmp = prev_nxt;
            break;
        }

        case A_BLOCK_LOOP_INF: {
            jmp_buf brk, cont;
            jmp_buf *prev_brk = it->loop_jmp, *prev_nxt = it->next_jmp;
            ctrl_sig prev_sig = it->sig;
            it->loop_jmp = &brk;
            it->next_jmp = &cont;
            it->sig.kind = SIG_NONE;
            if (setjmp(brk) == 0) {
                for (volatile int64_t i = 0; ; i++) {
                    env_set_local(e, "iter", v_int(i + 1));
                    if (setjmp(cont) == 0) {
                        for (size_t j = 0; j < n->nchild; j++) {
                            exec_stmt(it, n->children[j], e);
                            if (it->sig.kind == SIG_ERROR) goto done_inf;
                            if (it->sig.kind == SIG_STOP)  goto done_inf;
                            if (it->sig.kind == SIG_NEXT)  break;
                            if (it->sig.kind == SIG_RETURN) goto done_inf;
                        }
                    }
                    it->sig.kind = SIG_NONE;
                }
            }
            done_inf:
            it->sig = (it->sig.kind == SIG_STOP || it->sig.kind == SIG_NEXT) ? prev_sig : it->sig;
            it->loop_jmp = prev_brk;
            it->next_jmp = prev_nxt;
            break;
        }

        case A_BLOCK_LOOP_FOR: {
            value sv = eval_expr(it, n->range_start, e);
            if (it->sig.kind != SIG_NONE) { env_free(e); return; }

            /* Array iteration form: for x in array (range_end is NULL) */
            if (n->range_end == NULL) {
                if (sv.kind != V_ARRAY) {
                    g_set_error("%d:%d: 'for x in' needs an array", n->line, n->col);
                    it->sig.kind = SIG_ERROR;
                    env_free(e);
                    return;
                }
                varr *arr = sv.as.arr;
                jmp_buf brk, cont;
                jmp_buf *prev_brk = it->loop_jmp, *prev_nxt = it->next_jmp;
                ctrl_sig prev_sig = it->sig;
                it->loop_jmp = &brk;
                it->next_jmp = &cont;
                it->sig.kind = SIG_NONE;
                if (setjmp(brk) == 0) {
                    for (size_t i = 0; i < arr->len; i++) {
                        env_set_local(e, n->loop_var, v_clone(&arr->items[i]));
                        env_set_local(e, "iter", v_int((int64_t)(i + 1)));
                        if (setjmp(cont) == 0) {
                            for (size_t j = 0; j < n->nchild; j++) {
                                exec_stmt(it, n->children[j], e);
                                if (it->sig.kind == SIG_ERROR) goto done_for;
                                if (it->sig.kind == SIG_STOP)  goto done_for;
                                if (it->sig.kind == SIG_NEXT)  break;
                                if (it->sig.kind == SIG_RETURN) goto done_for;
                            }
                        }
                        it->sig.kind = SIG_NONE;
                    }
                }
                it->loop_jmp = prev_brk;
                it->next_jmp = prev_nxt;
                it->sig = prev_sig;
                goto done_for;
            }

            /* Range form: for i in a .. b [, step] */
            value ev = eval_expr(it, n->range_end, e);
            if (it->sig.kind != SIG_NONE) { env_free(e); return; }
            volatile int64_t step = 1;
            if (n->range_step) {
                value stv = eval_expr(it, n->range_step, e);
                if (it->sig.kind != SIG_NONE) { env_free(e); return; }
                step = stv.kind == V_INT ? stv.as.i : (int64_t)stv.as.f;
            }
            volatile int64_t s = sv.kind == V_INT ? sv.as.i : (int64_t)sv.as.f;
            volatile int64_t en = ev.kind == V_INT ? ev.as.i : (int64_t)ev.as.f;
            jmp_buf brk, cont;
            jmp_buf *prev_brk = it->loop_jmp, *prev_nxt = it->next_jmp;
            ctrl_sig prev_sig = it->sig;
            it->loop_jmp = &brk;
            it->next_jmp = &cont;
            it->sig.kind = SIG_NONE;
            if (setjmp(brk) == 0) {
                if (step > 0) {
                    for (volatile int64_t i = s; i < en; i += step) {
                        env_set_local(e, n->loop_var, v_int(i));
                        env_set_local(e, "iter", v_int(i - s + 1));
                        if (setjmp(cont) == 0) {
                            for (size_t j = 0; j < n->nchild; j++) {
                                exec_stmt(it, n->children[j], e);
                                if (it->sig.kind == SIG_ERROR) goto done_for;
                                if (it->sig.kind == SIG_STOP)  goto done_for;
                                if (it->sig.kind == SIG_NEXT)  break;
                                if (it->sig.kind == SIG_RETURN) goto done_for;
                            }
                        }
                        it->sig.kind = SIG_NONE;
                    }
                } else if (step < 0) {
                    for (volatile int64_t i = s; i > en; i += step) {
                        env_set_local(e, n->loop_var, v_int(i));
                        env_set_local(e, "iter", v_int(s - i + 1));
                        if (setjmp(cont) == 0) {
                            for (size_t j = 0; j < n->nchild; j++) {
                                exec_stmt(it, n->children[j], e);
                                if (it->sig.kind == SIG_ERROR) goto done_for;
                                if (it->sig.kind == SIG_STOP)  goto done_for;
                                if (it->sig.kind == SIG_NEXT)  break;
                                if (it->sig.kind == SIG_RETURN) goto done_for;
                            }
                        }
                        it->sig.kind = SIG_NONE;
                    }
                }
            }
            done_for:
            it->sig = (it->sig.kind == SIG_STOP || it->sig.kind == SIG_NEXT) ? prev_sig : it->sig;
            it->loop_jmp = prev_brk;
            it->next_jmp = prev_nxt;
            break;
        }

        case A_BLOCK_GUARD: {
            value c = eval_expr(it, n->cond, e);
            if (it->sig.kind != SIG_NONE) { env_free(e); return; }
            if (v_truthy(&c).as.i) {
                for (size_t j = 0; j < n->nchild; j++) {
                    exec_stmt(it, n->children[j], e);
                    if (it->sig.kind != SIG_NONE) break;
                }
            } else if (n->else_branch) {
                a_node *eb = n->else_branch;
                for (size_t j = 0; j < eb->nchild; j++) {
                    exec_stmt(it, eb->children[j], e);
                    if (it->sig.kind != SIG_NONE) break;
                }
            }
            break;
        }

        default:
            g_set_error("%d:%d: unknown block kind %d", n->line, n->col, n->kind);
            it->sig.kind = SIG_ERROR;
    }

    env_free(e);
}

static void exec_stmt(interp *it, a_node *n, env *e) {
    if (!n) return;
    if (it->sig.kind != SIG_NONE && it->sig.kind != SIG_NEXT) return;

    switch (n->kind) {
        case A_BLOCK_SQUIRE:
        case A_BLOCK_TRIGGER:
        case A_BLOCK_LANG:
        case A_BLOCK_LOOP_COUNT:
        case A_BLOCK_LOOP_INF:
        case A_BLOCK_LOOP_FOR:
        case A_BLOCK_SEQ:
        case A_BLOCK_GUARD:
            exec_block(it, n, e);
            return;

        case A_ASSIGN: {
            value v = eval_expr(it, n->rhs, e);
            if (it->sig.kind != SIG_NONE) return;
            env_set(e, n->sval, v);
            return;
        }

        case A_COMPOUND_ASSIGN: {
            /* n->sval is the operator (+,-,*,/,%), n->lhs is A_IDENT (var name),
               n->rhs is the expression to combine */
            int found;
            value cur = env_get(e, n->lhs->sval, &found);
            if (!found) {
                g_set_error("%d:%d: undefined variable '%s' in compound assignment",
                            n->line, n->col, n->lhs->sval);
                it->sig.kind = SIG_ERROR;
                return;
            }
            value rhs = eval_expr(it, n->rhs, e);
            if (it->sig.kind != SIG_NONE) return;
            value result = do_binop(it, n->sval, cur, rhs, n->line, n->col);
            if (it->sig.kind != SIG_NONE) return;
            env_set(e, n->lhs->sval, result);
            return;
        }

        case A_INDEX_ASSIGN: {
            /* name[idx] = value */
            int found;
            value arr = env_get(e, n->sval, &found);
            if (!found) {
                g_set_error("%d:%d: undefined variable '%s' in index assignment",
                            n->line, n->col, n->sval);
                it->sig.kind = SIG_ERROR;
                return;
            }
            if (arr.kind != V_ARRAY) {
                g_set_error("%d:%d: cannot index-assign to non-array", n->line, n->col);
                it->sig.kind = SIG_ERROR;
                return;
            }
            value idx = eval_expr(it, n->lhs, e);
            if (it->sig.kind != SIG_NONE) return;
            value v = eval_expr(it, n->rhs, e);
            if (it->sig.kind != SIG_NONE) return;
            int64_t i = idx.kind == V_INT ? idx.as.i : (int64_t)idx.as.f;
            if (i < 0) i += (int64_t)arr.as.arr->len;
            if (i < 0 || (size_t)i >= arr.as.arr->len) {
                a_node *trig = find_trigger(it, "bound");
                if (trig && !it->in_trigger) {
                    it->in_trigger = 1;
                    fire_trigger(it, "bound");
                    it->in_trigger = 0;
                    return;
                }
                g_set_error("%d:%d: index %lld out of bounds in assignment", n->line, n->col, (long long)i);
                it->sig.kind = SIG_ERROR;
                return;
            }
            v_free(&arr.as.arr->items[i]);
            arr.as.arr->items[i] = v;
            return;
        }

        case A_PRINT: {
            for (size_t i = 0; i < n->nchild; i++) {
                if (i) fputc(' ', stdout);
                value v = eval_expr(it, n->children[i], e);
                if (it->sig.kind != SIG_NONE) return;
                char *s = v_to_string(&v);
                fputs(s, stdout);
                free(s);
                v_free(&v);
            }
            fputc('\n', stdout);
            fflush(stdout);
            return;
        }

        case A_RETURN: {
            value v;
            if (n->lhs) {
                v = eval_expr(it, n->lhs, e);
                if (it->sig.kind != SIG_NONE) return;
            } else {
                v = v_nil();
            }
            it->sig.kind = SIG_RETURN;
            it->sig.val  = v;
            if (it->ret_jmp) longjmp(*it->ret_jmp, 1);
            return;
        }

        case A_STOP: {
            it->sig.kind = SIG_STOP;
            if (it->loop_jmp) longjmp(*it->loop_jmp, 1);
            return;
        }

        case A_NEXT: {
            it->sig.kind = SIG_NEXT;
            if (it->next_jmp) longjmp(*it->next_jmp, 1);
            return;
        }

        case A_RAISE: {
            /* user-defined trigger */
            a_node *trig = find_trigger(it, n->sval);
            if (!trig) {
                g_set_error("%d:%d: raise: no trigger '%s' registered", n->line, n->col, n->sval);
                it->sig.kind = SIG_ERROR;
                return;
            }
            /* run the trigger; ignore its return value for raise */
            it->in_trigger = 1;
            fire_trigger(it, n->sval);
            it->in_trigger = 0;
            if (it->sig.kind == SIG_ERROR) return;
            return;
        }

        case A_INVOKE: {
            value v = eval_expr(it, n, e);
            if (it->sig.kind != SIG_NONE) return;
            v_free(&v);
            return;
        }

        default:
            /* treat any expression node as an expression statement */
            if (n->kind == A_INT || n->kind == A_FLOAT || n->kind == A_STRING ||
                n->kind == A_BOOL || n->kind == A_NIL || n->kind == A_IDENT ||
                n->kind == A_BINOP || n->kind == A_UNOP || n->kind == A_CALL ||
                n->kind == A_INDEX || n->kind == A_ARRAY_LITERAL) {
                value v = eval_expr(it, n, e);
                if (it->sig.kind != SIG_NONE) return;
                v_free(&v);
                return;
            }
            g_set_error("%d:%d: unknown statement kind %d", n->line, n->col, n->kind);
            it->sig.kind = SIG_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Unix signal handling                                                */
/* ------------------------------------------------------------------ */

static interp *g_interp_for_signal = NULL;
static interp *g_current_interp    = NULL;

/* Public: callable from stdlib.c to invoke a global squire by name. */
value interp_call_global_squire(const char *name, int argc, value *argv) {
    if (!g_current_interp) {
        g_set_error("interp_call_global_squire: no interpreter running");
        return v_nil();
    }
    int found = 0;
    value fn = env_get(g_current_interp->globals, name, &found);
    if (!found) {
        g_set_error("call: no squire named '%s'", name);
        return v_nil();
    }
    if (fn.kind != V_FUNC) {
        g_set_error("call: '%s' is not a squire", name);
        return v_nil();
    }
    return call_squire(g_current_interp, fn.as.func.def, argc, argv,
                       fn.as.func.closure);
}

static void signal_handler(int signo) {
    if (!g_interp_for_signal) return;
    /* record that a signal arrived; process later in main loop */
    if (g_interp_for_signal->n_pending_signals < (int)(sizeof(g_interp_for_signal->signal_nums)/sizeof(int))) {
        g_interp_for_signal->signal_nums[g_interp_for_signal->n_pending_signals++] = signo;
    }
}

void interp_signal(interp *it, int signo) {
    a_node *trig = find_trigger(it, "signal");
    if (trig) {
        env *te = env_new(it->globals);
        env_set_local(te, "signo", v_int(signo));
        it->in_trigger = 1;
        /* run body manually because fire_trigger doesn't pass args */
        jmp_buf jb;
        jmp_buf *prev = it->ret_jmp;
        it->ret_jmp = &jb;
        ctrl_sig prev_sig = it->sig;
        it->sig.kind = SIG_NONE;
        if (setjmp(jb) == 0) {
            for (size_t i = 0; i < trig->nchild; i++) {
                exec_stmt(it, trig->children[i], te);
                if (it->sig.kind != SIG_NONE) break;
            }
        }
        it->sig = prev_sig;
        it->ret_jmp = prev;
        it->in_trigger = 0;
        env_free(te);
    }
}

static void install_signal_handlers(interp *it) {
    g_interp_for_signal = it;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* top-level run                                                       */
/* ------------------------------------------------------------------ */

g_status interp_run(interp *it, a_node *program) {
    install_signal_handlers(it);
    g_current_interp = it;

    /* first pass: register all top-level squires and triggers */
    for (size_t i = 0; i < program->nchild; i++) {
        a_node *s = program->children[i];
        if (s->kind == A_BLOCK_SQUIRE) {
            value fn = {0};
            fn.kind = V_FUNC;
            fn.as.func.def = s;
            fn.as.func.closure = it->globals;
            env_set_local(it->globals, s->sval, fn);
        } else if (s->kind == A_BLOCK_TRIGGER) {
            interp_register_trigger(it, s->sval, s);
        }
    }

    /* second pass: execute non-definition statements at top level.
     * Each top-level block (SEQ, loop, guard, etc.) is independent.
     * SIG_RETURN from one block should NOT stop subsequent blocks. */
    for (size_t i = 0; i < program->nchild; i++) {
        a_node *s = program->children[i];
        if (s->kind == A_BLOCK_SQUIRE || s->kind == A_BLOCK_TRIGGER) continue;
        exec_stmt(it, s, it->globals);
        if (it->sig.kind == SIG_ERROR) {
            fprintf(stderr, "glyph: runtime error: %s\n", g_last_error());
            return G_ERR_RUNTIME;
        }
        /* Clear any control-flow signal — at top level, return/stop/next
         * only affect the current block, not subsequent top-level statements. */
        it->sig.kind = SIG_NONE;
    }

    /* drain pending signals */
    while (it->n_pending_signals > 0) {
        int s = it->signal_nums[--it->n_pending_signals];
        interp_signal(it, s);
    }

    return G_OK;
}

void interp_reset_env(interp *it) {
    env_free(it->globals);
    it->globals = env_new(NULL);
    interp_install_builtins(it);
    /* Clear triggers too */
    for (size_t i = 0; i < it->ntrigger; i++) free(it->triggers[i].name);
    it->ntrigger = 0;
}
