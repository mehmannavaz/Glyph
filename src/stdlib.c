/* stdlib.c — Glyph standard library.
 *
 * Plan 9 / Unix philosophy: small, sharp tools. Each section is one
 * concern, registered into the interpreter as a set of native builtins.
 *
 * Sections:
 *   1. String operations (str_*)
 *   2. Array operations (arr_*)
 *   3. Dict operations (dict_*)
 *   4. File I/O (file_*)
 *   5. Math (math_*)
 *   6. Time (time_*)
 *   7. Process (proc_*)
 *   8. Functional (call, apply)
 *   9. Regex (re_*) — POSIX extended regex
 *  10. Type conversion (to_*)
 *
 * Every function is exposed via a getter that returns the native_fn
 * pointer, so interp.c (which owns the interp struct) can register
 * them. The getters are declared in glyph.h.
 */
#include "glyph.h"
#include "platform.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <regex.h>

/* Helper: argument validation */
static int need_args(int argc, int n, const char *name) {
    if (argc != n) {
        g_set_error("%s() takes %d arg(s), got %d", name, n, argc);
        return 0;
    }
    return 1;
}

/* Helper: convert any value to a C double */
static double to_double(const value *v) {
    if (v->kind == V_FLOAT) return v->as.f;
    if (v->kind == V_INT)   return (double)v->as.i;
    if (v->kind == V_STRING && v->as.s) return atof(v->as.s);
    return 0.0;
}

/* Helper: convert any value to a C int64 */
static int64_t to_int(const value *v) {
    if (v->kind == V_INT)    return v->as.i;
    if (v->kind == V_FLOAT)  return (int64_t)v->as.f;
    if (v->kind == V_STRING && v->as.s) return strtoll(v->as.s, NULL, 10);
    if (v->kind == V_PTR)    return (int64_t)v->as.ptr;
    return 0;
}

/* Helper: get a string arg, with NULL safety */
static const char *str_arg(const value *v) {
    return (v->kind == V_STRING && v->as.s) ? v->as.s : "";
}

/* ================================================================== */
/* STRINGS                                                            */
/* ================================================================== */

static value n_str_find(int argc, value *argv) {
    if (!need_args(argc, 2, "str_find")) return v_int(-1);
    const char *hay = str_arg(&argv[0]);
    const char *needle = str_arg(&argv[1]);
    const char *p = strstr(hay, needle);
    if (!p) return v_int(-1);
    return v_int((int64_t)(p - hay));
}

static value n_str_find_from(int argc, value *argv) {
    if (!need_args(argc, 3, "str_find_from")) return v_int(-1);
    const char *hay = str_arg(&argv[0]);
    const char *needle = str_arg(&argv[1]);
    int64_t start = to_int(&argv[2]);
    if (start < 0) start = 0;
    size_t hlen = strlen(hay);
    if ((size_t)start > hlen) return v_int(-1);
    const char *p = strstr(hay + start, needle);
    if (!p) return v_int(-1);
    return v_int((int64_t)(p - hay));
}

static value n_str_slice(int argc, value *argv) {
    if (!need_args(argc, 3, "str_slice")) return v_str("");
    const char *s = str_arg(&argv[0]);
    int64_t start = to_int(&argv[1]);
    int64_t end   = to_int(&argv[2]);
    int64_t len = (int64_t)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return v_str("");
    /* substring and return */
    size_t n = (size_t)(end - start);
    char *buf = malloc(n + 1);
    memcpy(buf, s + start, n);
    buf[n] = '\0';
    return v_str_take(buf);
}

static value n_str_split(int argc, value *argv) {
    if (!need_args(argc, 2, "str_split")) return v_arr();
    const char *s = str_arg(&argv[0]);
    const char *delim = str_arg(&argv[1]);
    value arr = v_arr();
    size_t dlen = strlen(delim);
    if (dlen == 0) {
        /* split into characters */
        for (const char *p = s; *p; p++) {
            char buf[2] = { *p, 0 };
            /* grow array */
            varr *a = arr.as.arr;
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str(buf);
        }
        return arr;
    }
    const char *p = s;
    while (*p) {
        const char *found = strstr(p, delim);
        if (!found) {
            /* rest of string */
            varr *a = arr.as.arr;
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str(p);
            break;
        }
        size_t n = (size_t)(found - p);
        char *buf = malloc(n + 1);
        memcpy(buf, p, n);
        buf[n] = '\0';
        varr *a = arr.as.arr;
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v_str_take(buf);
        p = found + dlen;
        /* if string ends with delimiter, add an empty final element */
        if (!*p) {
            a = arr.as.arr;
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str("");
            break;
        }
    }
    return arr;
}

static value n_str_join(int argc, value *argv) {
    if (!need_args(argc, 2, "str_join")) return v_str("");
    /* Accept either (arr, delim) or (delim, arr) — be liberal in what
     * you accept, conservative in what you emit (Postel's law). */
    varr *a;
    const char *delim;
    if (argv[0].kind == V_ARRAY) {
        a = argv[0].as.arr;
        delim = str_arg(&argv[1]);
    } else if (argv[1].kind == V_ARRAY) {
        a = argv[1].as.arr;
        delim = str_arg(&argv[0]);
    } else {
        g_set_error("str_join: one arg must be an array");
        return v_str("");
    }
    sbuf sb; sbuf_init(&sb);
    for (size_t i = 0; i < a->len; i++) {
        if (i) sbuf_puts(&sb, delim);
        char *s = v_to_string(&a->items[i]);
        sbuf_puts(&sb, s);
        free(s);
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_str_replace(int argc, value *argv) {
    if (!need_args(argc, 3, "str_replace")) return v_str("");
    const char *s = str_arg(&argv[0]);
    const char *old = str_arg(&argv[1]);
    const char *new_s = str_arg(&argv[2]);
    size_t olen = strlen(old);
    if (olen == 0) return v_str(s);
    const char *p = strstr(s, old);
    if (!p) return v_str(s);
    sbuf sb; sbuf_init(&sb);
    sbuf_putn(&sb, s, (size_t)(p - s));
    sbuf_puts(&sb, new_s);
    sbuf_puts(&sb, p + olen);
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_str_replace_all(int argc, value *argv) {
    if (!need_args(argc, 3, "str_replace_all")) return v_str("");
    const char *s = str_arg(&argv[0]);
    const char *old = str_arg(&argv[1]);
    const char *new_s = str_arg(&argv[2]);
    size_t olen = strlen(old);
    if (olen == 0) return v_str(s);
    sbuf sb; sbuf_init(&sb);
    const char *p = s;
    while (*p) {
        const char *found = strstr(p, old);
        if (!found) {
            sbuf_puts(&sb, p);
            break;
        }
        sbuf_putn(&sb, p, (size_t)(found - p));
        sbuf_puts(&sb, new_s);
        p = found + olen;
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_str_trim(int argc, value *argv) {
    if (!need_args(argc, 1, "str_trim")) return v_str("");
    const char *s = str_arg(&argv[0]);
    while (*s && isspace((unsigned char)*s)) s++;
    const char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - s);
    char *buf = malloc(n + 1);
    memcpy(buf, s, n);
    buf[n] = '\0';
    return v_str_take(buf);
}

static value n_str_trim_left(int argc, value *argv) {
    if (!need_args(argc, 1, "str_trim_left")) return v_str("");
    const char *s = str_arg(&argv[0]);
    while (*s && isspace((unsigned char)*s)) s++;
    return v_str(s);
}

static value n_str_trim_right(int argc, value *argv) {
    if (!need_args(argc, 1, "str_trim_right")) return v_str("");
    const char *s = str_arg(&argv[0]);
    const char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - s);
    char *buf = malloc(n + 1);
    memcpy(buf, s, n);
    buf[n] = '\0';
    return v_str_take(buf);
}

static value n_str_upper(int argc, value *argv) {
    if (!need_args(argc, 1, "str_upper")) return v_str("");
    const char *s = str_arg(&argv[0]);
    size_t n = strlen(s);
    char *buf = malloc(n + 1);
    for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[n] = '\0';
    return v_str_take(buf);
}

static value n_str_lower(int argc, value *argv) {
    if (!need_args(argc, 1, "str_lower")) return v_str("");
    const char *s = str_arg(&argv[0]);
    size_t n = strlen(s);
    char *buf = malloc(n + 1);
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = '\0';
    return v_str_take(buf);
}

static value n_str_starts_with(int argc, value *argv) {
    if (!need_args(argc, 2, "str_starts_with")) return v_int(0);
    const char *s = str_arg(&argv[0]);
    const char *p = str_arg(&argv[1]);
    size_t plen = strlen(p);
    return v_int(strncmp(s, p, plen) == 0 ? 1 : 0);
}

static value n_str_ends_with(int argc, value *argv) {
    if (!need_args(argc, 2, "str_ends_with")) return v_int(0);
    const char *s = str_arg(&argv[0]);
    const char *p = str_arg(&argv[1]);
    size_t slen = strlen(s);
    size_t plen = strlen(p);
    if (plen > slen) return v_int(0);
    return v_int(strcmp(s + slen - plen, p) == 0 ? 1 : 0);
}

static value n_str_contains(int argc, value *argv) {
    if (!need_args(argc, 2, "str_contains")) return v_int(0);
    const char *s = str_arg(&argv[0]);
    const char *p = str_arg(&argv[1]);
    return v_int(strstr(s, p) ? 1 : 0);
}

static value n_str_repeat(int argc, value *argv) {
    if (!need_args(argc, 2, "str_repeat")) return v_str("");
    const char *s = str_arg(&argv[0]);
    int64_t n = to_int(&argv[1]);
    if (n < 0) n = 0;
    size_t slen = strlen(s);
    if (slen == 0 || n == 0) return v_str("");
    size_t total = slen * (size_t)n;
    char *buf = malloc(total + 1);
    for (int64_t i = 0; i < n; i++) memcpy(buf + i * slen, s, slen);
    buf[total] = '\0';
    return v_str_take(buf);
}

static value n_str_reverse(int argc, value *argv) {
    if (!need_args(argc, 1, "str_reverse")) return v_str("");
    const char *s = str_arg(&argv[0]);
    size_t n = strlen(s);
    char *buf = malloc(n + 1);
    for (size_t i = 0; i < n; i++) buf[i] = s[n - 1 - i];
    buf[n] = '\0';
    return v_str_take(buf);
}

static value n_str_chars(int argc, value *argv) {
    if (!need_args(argc, 1, "str_chars")) return v_arr();
    const char *s = str_arg(&argv[0]);
    value arr = v_arr();
    varr *a = arr.as.arr;
    for (const char *p = s; *p; p++) {
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        char buf[2] = { *p, 0 };
        a->items[a->len++] = v_str(buf);
    }
    return arr;
}

static value n_str_bytes(int argc, value *argv) {
    if (!need_args(argc, 1, "str_bytes")) return v_arr();
    const char *s = str_arg(&argv[0]);
    value arr = v_arr();
    varr *a = arr.as.arr;
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v_int((int64_t)*p);
    }
    return arr;
}

static value n_str_from_bytes(int argc, value *argv) {
    if (!need_args(argc, 1, "str_from_bytes") || argv[0].kind != V_ARRAY) {
        g_set_error("str_from_bytes: needs an array of ints");
        return v_str("");
    }
    varr *a = argv[0].as.arr;
    char *buf = malloc(a->len + 1);
    for (size_t i = 0; i < a->len; i++) {
        buf[i] = (char)(unsigned char)to_int(&a->items[i]);
    }
    buf[a->len] = '\0';
    return v_str_take(buf);
}

static value n_str_to_int(int argc, value *argv) {
    if (argc < 1 || argc > 2) {
        g_set_error("str_to_int(s, base?) takes 1 or 2 args");
        return v_int(0);
    }
    const char *s = str_arg(&argv[0]);
    int base = 10;
    if (argc == 2) base = (int)to_int(&argv[1]);
    if (base < 2 || base > 36) {
        g_set_error("str_to_int: base must be 2..36");
        return v_int(0);
    }
    return v_int((int64_t)strtoll(s, NULL, base));
}

static value n_str_to_float(int argc, value *argv) {
    if (!need_args(argc, 1, "str_to_float")) return v_float(0);
    return v_float(atof(str_arg(&argv[0])));
}

static value n_int_to_str(int argc, value *argv) {
    if (argc < 1 || argc > 2) {
        g_set_error("int_to_str(n, base?) takes 1 or 2 args");
        return v_str("");
    }
    int64_t n = to_int(&argv[0]);
    int base = 10;
    if (argc == 2) base = (int)to_int(&argv[1]);
    char buf[64];
    if (base == 10) {
        snprintf(buf, sizeof buf, "%ld", (long)n);
    } else if (base == 16) {
        snprintf(buf, sizeof buf, "%lx", (unsigned long)n);
    } else if (base == 8) {
        snprintf(buf, sizeof buf, "%lo", (unsigned long)n);
    } else if (base == 2) {
        /* manual binary conversion */
        unsigned long un = (unsigned long)n;
        char tmp[65];
        int i = 0;
        if (un == 0) tmp[i++] = '0';
        while (un) { tmp[i++] = '0' + (un & 1); un >>= 1; }
        tmp[i] = 0;
        /* reverse */
        int j = 0;
        while (i > 0) buf[j++] = tmp[--i];
        buf[j] = 0;
    } else {
        g_set_error("int_to_str: base %d not supported (use 2/8/10/16)", base);
        return v_str("");
    }
    return v_str(buf);
}

static value n_float_to_str(int argc, value *argv) {
    if (!need_args(argc, 1, "float_to_str")) return v_str("");
    char buf[64];
    snprintf(buf, sizeof buf, "%g", to_double(&argv[0]));
    return v_str(buf);
}

static value n_str_format(int argc, value *argv) {
    if (argc < 1 || argv[0].kind != V_STRING) {
        g_set_error("str_format(fmt, args...) needs a string format");
        return v_str("");
    }
    const char *fmt = argv[0].as.s;
    sbuf sb; sbuf_init(&sb);
    int argi = 1;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sbuf_putc(&sb, *p); continue; }
        p++;
        if (!*p) break;
        /* Parse optional flags + width + precision */
        char spec[32];
        int sp = 0;
        spec[sp++] = '%';
        /* flags */
        while (*p && (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')) {
            spec[sp++] = *p++;
        }
        /* width */
        while (*p && isdigit((unsigned char)*p)) spec[sp++] = *p++;
        /* precision */
        if (*p == '.') {
            spec[sp++] = *p++;
            while (*p && isdigit((unsigned char)*p)) spec[sp++] = *p++;
        }
        if (!*p) break;
        char conv = *p;
        /* Note: conv is NOT added to spec here — the conversion char is
         * appended by each case below (with the right length modifier). */
        spec[sp] = '\0';
        switch (conv) {
            case 'd': case 'i': {
                char fmt2[40];
                snprintf(fmt2, sizeof fmt2, "%sld", spec);
                if (argi < argc) sbuf_printf(&sb, fmt2, (long)to_int(&argv[argi++]));
                break;
            }
            case 'f': case 'g': case 'e': {
                char fmt2[40];
                snprintf(fmt2, sizeof fmt2, "%s%c", spec, conv);
                if (argi < argc) sbuf_printf(&sb, fmt2, to_double(&argv[argi++]));
                break;
            }
            case 's': {
                char fmt2[40];
                snprintf(fmt2, sizeof fmt2, "%ss", spec);
                if (argi < argc) {
                    char *s = v_to_string(&argv[argi++]);
                    sbuf_printf(&sb, fmt2, s);
                    free(s);
                }
                break;
            }
            case 'x': {
                char fmt2[40];
                snprintf(fmt2, sizeof fmt2, "%slx", spec);
                if (argi < argc) sbuf_printf(&sb, fmt2, (unsigned long)to_int(&argv[argi++]));
                break;
            }
            case 'o': {
                char fmt2[40];
                snprintf(fmt2, sizeof fmt2, "%slo", spec);
                if (argi < argc) sbuf_printf(&sb, fmt2, (unsigned long)to_int(&argv[argi++]));
                break;
            }
            case 'c':
                if (argi < argc) {
                    char *s = v_to_string(&argv[argi++]);
                    if (s[0]) sbuf_putc(&sb, s[0]);
                    free(s);
                }
                break;
            case '%':
                sbuf_putc(&sb, '%');
                break;
            default:
                sbuf_putc(&sb, '%');
                sbuf_puts(&sb, spec+1);
                sbuf_putc(&sb, conv);
                break;
        }
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

/* ================================================================== */
/* ARRAYS                                                             */
/* ================================================================== */

static value n_arr_push(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_push") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_push(arr, val) needs an array");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->items = realloc(a->items, a->cap * sizeof(value));
    }
    a->items[a->len++] = v_clone(&argv[1]);
    return argv[0];
}

static value n_arr_pop(int argc, value *argv) {
    if (!need_args(argc, 1, "arr_pop") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_pop(arr) needs an array");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    if (a->len == 0) return v_nil();
    return a->items[--a->len];
}

static value n_arr_shift(int argc, value *argv) {
    if (!need_args(argc, 1, "arr_shift") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_shift(arr) needs an array");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    if (a->len == 0) return v_nil();
    value v = a->items[0];
    for (size_t i = 1; i < a->len; i++) a->items[i-1] = a->items[i];
    a->len--;
    return v;
}

static value n_arr_unshift(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_unshift") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_unshift(arr, val) needs an array");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->items = realloc(a->items, a->cap * sizeof(value));
    }
    for (size_t i = a->len; i > 0; i--) a->items[i] = a->items[i-1];
    a->items[0] = v_clone(&argv[1]);
    a->len++;
    return argv[0];
}

static value n_arr_map(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_map") ||
        argv[0].kind != V_ARRAY || argv[1].kind != V_STRING) {
        g_set_error("arr_map(arr, squire_name) needs an array and a string");
        return v_arr();
    }
    varr *a = argv[0].as.arr;
    const char *fn = argv[1].as.s;
    value out = v_arr();
    varr *o = out.as.arr;
    for (size_t i = 0; i < a->len; i++) {
        value args[1] = { a->items[i] };
        value r = interp_call_global_squire(fn, 1, args);
        if (o->len >= o->cap) {
            o->cap = o->cap ? o->cap * 2 : 8;
            o->items = realloc(o->items, o->cap * sizeof(value));
        }
        o->items[o->len++] = r;
    }
    return out;
}

static value n_arr_filter(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_filter") ||
        argv[0].kind != V_ARRAY || argv[1].kind != V_STRING) {
        g_set_error("arr_filter(arr, squire_name) needs an array and a string");
        return v_arr();
    }
    varr *a = argv[0].as.arr;
    const char *fn = argv[1].as.s;
    value out = v_arr();
    varr *o = out.as.arr;
    for (size_t i = 0; i < a->len; i++) {
        value args[1] = { a->items[i] };
        value r = interp_call_global_squire(fn, 1, args);
        if (v_truthy(&r).as.i) {
            if (o->len >= o->cap) {
                o->cap = o->cap ? o->cap * 2 : 8;
                o->items = realloc(o->items, o->cap * sizeof(value));
            }
            o->items[o->len++] = a->items[i];
        }
    }
    return out;
}

static value n_arr_reduce(int argc, value *argv) {
    if (argc != 3 || argv[0].kind != V_ARRAY || argv[1].kind != V_STRING) {
        g_set_error("arr_reduce(arr, squire_name, init) needs (array, string, value)");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    const char *fn = argv[1].as.s;
    value acc = v_clone(&argv[2]);
    for (size_t i = 0; i < a->len; i++) {
        value args[2] = { acc, a->items[i] };
        acc = interp_call_global_squire(fn, 2, args);
    }
    return acc;
}

/* qsort comparison function */
static const char *g_sort_fn_name = NULL;
static int sort_cmp(const void *pa, const void *pb) {
    value a = *(const value*)pa;
    value b = *(const value*)pb;
    if (g_sort_fn_name && *g_sort_fn_name) {
        value args[2] = { a, b };
        value r = interp_call_global_squire(g_sort_fn_name, 2, args);
        int64_t n = to_int(&r);
        return n < 0 ? -1 : (n > 0 ? 1 : 0);
    }
    /* default: lexicographic by v_lt */
    if (v_lt(&a, &b)) return -1;
    if (v_lt(&b, &a)) return 1;
    return 0;
}

static value n_arr_sort(int argc, value *argv) {
    if (argc < 1 || argc > 2 || argv[0].kind != V_ARRAY) {
        g_set_error("arr_sort(arr, squire_name?) needs an array");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    if (argc == 2 && argv[1].kind == V_STRING) {
        g_sort_fn_name = argv[1].as.s;
    } else {
        g_sort_fn_name = NULL;
    }
    qsort(a->items, a->len, sizeof(value), sort_cmp);
    g_sort_fn_name = NULL;
    return argv[0];
}

static value n_arr_reverse(int argc, value *argv) {
    if (!need_args(argc, 1, "arr_reverse") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_reverse(arr) needs an array");
        return v_nil();
    }
    varr *a = argv[0].as.arr;
    for (size_t i = 0, j = a->len - 1; i < j; i++, j--) {
        value tmp = a->items[i];
        a->items[i] = a->items[j];
        a->items[j] = tmp;
    }
    return argv[0];
}

static value n_arr_concat(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_concat") ||
        argv[0].kind != V_ARRAY || argv[1].kind != V_ARRAY) {
        g_set_error("arr_concat(a, b) needs two arrays");
        return v_arr();
    }
    varr *a = argv[0].as.arr;
    varr *b = argv[1].as.arr;
    value out = v_arr();
    varr *o = out.as.arr;
    o->cap = a->len + b->len;
    o->items = realloc(o->items, o->cap * sizeof(value));
    for (size_t i = 0; i < a->len; i++) o->items[o->len++] = a->items[i];
    for (size_t i = 0; i < b->len; i++) o->items[o->len++] = b->items[i];
    return out;
}

static value n_arr_slice(int argc, value *argv) {
    if (!need_args(argc, 3, "arr_slice") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_slice(arr, start, end) needs an array and two ints");
        return v_arr();
    }
    varr *a = argv[0].as.arr;
    int64_t start = to_int(&argv[1]);
    int64_t end = to_int(&argv[2]);
    if (start < 0) start = 0;
    if (end > (int64_t)a->len) end = (int64_t)a->len;
    if (start >= end) return v_arr();
    value out = v_arr();
    varr *o = out.as.arr;
    o->cap = (size_t)(end - start);
    o->items = realloc(o->items, o->cap * sizeof(value));
    for (int64_t i = start; i < end; i++) o->items[o->len++] = a->items[i];
    return out;
}

static value n_arr_find(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_find") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_find(arr, val) needs an array");
        return v_int(-1);
    }
    varr *a = argv[0].as.arr;
    for (size_t i = 0; i < a->len; i++) {
        if (v_eq(&a->items[i], &argv[1])) return v_int((int64_t)i);
    }
    return v_int(-1);
}

static value n_arr_contains(int argc, value *argv) {
    if (!need_args(argc, 2, "arr_contains") || argv[0].kind != V_ARRAY) {
        g_set_error("arr_contains(arr, val) needs an array");
        return v_int(0);
    }
    varr *a = argv[0].as.arr;
    for (size_t i = 0; i < a->len; i++) {
        if (v_eq(&a->items[i], &argv[1])) return v_int(1);
    }
    return v_int(0);
}

/* ================================================================== */
/* DICTS                                                              */
/* ================================================================== */

static value n_dict_new(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_dict_value();
}

static value n_dict_set(int argc, value *argv) {
    if (!need_args(argc, 3, "dict_set") ||
        argv[0].kind != V_DICT || argv[1].kind != V_STRING) {
        g_set_error("dict_set(d, key, val) needs a dict and a string key");
        return v_nil();
    }
    dict_set(argv[0].as.dict, argv[1].as.s, v_clone(&argv[2]));
    return argv[0];
}

static value n_dict_get(int argc, value *argv) {
    if (!need_args(argc, 2, "dict_get") ||
        argv[0].kind != V_DICT || argv[1].kind != V_STRING) {
        g_set_error("dict_get(d, key) needs a dict and a string key");
        return v_nil();
    }
    int found;
    value v = dict_get(argv[0].as.dict, argv[1].as.s, &found);
    if (!found) return v_nil();
    return v_clone(&v);
}

static value n_dict_get_or(int argc, value *argv) {
    if (!need_args(argc, 3, "dict_get_or") ||
        argv[0].kind != V_DICT || argv[1].kind != V_STRING) {
        g_set_error("dict_get_or(d, key, default) needs a dict and a string key");
        return v_nil();
    }
    int found;
    value v = dict_get(argv[0].as.dict, argv[1].as.s, &found);
    if (!found) return v_clone(&argv[2]);
    return v_clone(&v);
}

static value n_dict_has(int argc, value *argv) {
    if (!need_args(argc, 2, "dict_has") ||
        argv[0].kind != V_DICT || argv[1].kind != V_STRING) {
        g_set_error("dict_has(d, key) needs a dict and a string key");
        return v_int(0);
    }
    return v_int(dict_has(argv[0].as.dict, argv[1].as.s));
}

static value n_dict_del(int argc, value *argv) {
    if (!need_args(argc, 2, "dict_del") ||
        argv[0].kind != V_DICT || argv[1].kind != V_STRING) {
        g_set_error("dict_del(d, key) needs a dict and a string key");
        return v_int(0);
    }
    return v_int(dict_del(argv[0].as.dict, argv[1].as.s));
}

static value n_dict_keys(int argc, value *argv) {
    if (!need_args(argc, 1, "dict_keys") || argv[0].kind != V_DICT) {
        g_set_error("dict_keys(d) needs a dict");
        return v_arr();
    }
    vdict *d = argv[0].as.dict;
    value arr = v_arr();
    varr *a = arr.as.arr;
    a->cap = d->len;
    a->items = realloc(a->items, a->cap * sizeof(value));
    for (size_t i = 0; i < d->cap; i++) {
        if (d->keys[i]) {
            a->items[a->len++] = v_str(d->keys[i]);
        }
    }
    return arr;
}

static value n_dict_vals(int argc, value *argv) {
    if (!need_args(argc, 1, "dict_vals") || argv[0].kind != V_DICT) {
        g_set_error("dict_vals(d) needs a dict");
        return v_arr();
    }
    vdict *d = argv[0].as.dict;
    value arr = v_arr();
    varr *a = arr.as.arr;
    a->cap = d->len;
    a->items = realloc(a->items, a->cap * sizeof(value));
    for (size_t i = 0; i < d->cap; i++) {
        if (d->keys[i]) {
            a->items[a->len++] = v_clone(&d->vals[i]);
        }
    }
    return arr;
}

static value n_dict_size(int argc, value *argv) {
    if (!need_args(argc, 1, "dict_size") || argv[0].kind != V_DICT) {
        g_set_error("dict_size(d) needs a dict");
        return v_int(0);
    }
    return v_int((int64_t)argv[0].as.dict->len);
}

static value n_dict_clear(int argc, value *argv) {
    if (!need_args(argc, 1, "dict_clear") || argv[0].kind != V_DICT) {
        g_set_error("dict_clear(d) needs a dict");
        return v_nil();
    }
    vdict *d = argv[0].as.dict;
    for (size_t i = 0; i < d->cap; i++) {
        if (d->keys[i]) {
            free(d->keys[i]);
            d->keys[i] = NULL;
            v_free(&d->vals[i]);
            d->vals[i] = v_nil();
        }
    }
    d->len = 0;
    return argv[0];
}

/* ================================================================== */
/* FILES                                                              */
/* ================================================================== */

static value n_file_open(int argc, value *argv) {
    if (!need_args(argc, 2, "file_open") ||
        argv[0].kind != V_STRING || argv[1].kind != V_STRING) {
        g_set_error("file_open(path, mode) needs two strings");
        return v_ptr(NULL);
    }
    FILE *f = fopen(argv[0].as.s, argv[1].as.s);
    if (!f) {
        g_set_error("file_open: %s: %s", argv[0].as.s, strerror(errno));
        return v_ptr(NULL);
    }
    return v_ptr(f);
}

static value n_file_close(int argc, value *argv) {
    if (!need_args(argc, 1, "file_close") || argv[0].kind != V_PTR) {
        g_set_error("file_close(handle) needs a ptr");
        return v_int(-1);
    }
    return v_int(fclose((FILE*)argv[0].as.ptr));
}

static value n_file_read(int argc, value *argv) {
    if (!need_args(argc, 2, "file_read") || argv[0].kind != V_PTR) {
        g_set_error("file_read(handle, n) needs a ptr and an int");
        return v_str("");
    }
    FILE *f = (FILE*)argv[0].as.ptr;
    int64_t n = to_int(&argv[1]);
    if (n < 0 || n > (1 << 24)) n = 4096;
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    return v_str_take(buf);
}

static value n_file_readln(int argc, value *argv) {
    if (!need_args(argc, 1, "file_readln") || argv[0].kind != V_PTR) {
        g_set_error("file_readln(handle) needs a ptr");
        return v_nil();
    }
    FILE *f = (FILE*)argv[0].as.ptr;
    sbuf sb; sbuf_init(&sb);
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        sbuf_putc(&sb, (char)c);
    }
    if (c == EOF && sb.len == 0) {
        /* EOF with no data — return nil to signal end of file */
        free(sb.data);
        return v_nil();
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_file_read_all(int argc, value *argv) {
    if (!need_args(argc, 1, "file_read_all") || argv[0].kind != V_PTR) {
        g_set_error("file_read_all(handle) needs a ptr");
        return v_str("");
    }
    FILE *f = (FILE*)argv[0].as.ptr;
    sbuf sb; sbuf_init(&sb);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sbuf_putn(&sb, buf, n);
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_file_write(int argc, value *argv) {
    if (!need_args(argc, 2, "file_write") ||
        argv[0].kind != V_PTR || argv[1].kind != V_STRING) {
        g_set_error("file_write(handle, str) needs a ptr and a string");
        return v_int(0);
    }
    FILE *f = (FILE*)argv[0].as.ptr;
    const char *s = argv[1].as.s;
    size_t len = strlen(s);
    size_t n = fwrite(s, 1, len, f);
    return v_int((int64_t)n);
}

static value n_file_writeln(int argc, value *argv) {
    if (!need_args(argc, 2, "file_writeln") ||
        argv[0].kind != V_PTR || argv[1].kind != V_STRING) {
        g_set_error("file_writeln(handle, str) needs a ptr and a string");
        return v_int(0);
    }
    FILE *f = (FILE*)argv[0].as.ptr;
    const char *s = argv[1].as.s;
    size_t len = strlen(s);
    size_t n = fwrite(s, 1, len, f);
    fputc('\n', f);
    return v_int((int64_t)(n + 1));
}

static value n_file_eof(int argc, value *argv) {
    if (!need_args(argc, 1, "file_eof") || argv[0].kind != V_PTR) {
        g_set_error("file_eof(handle) needs a ptr");
        return v_int(1);
    }
    return v_int(feof((FILE*)argv[0].as.ptr) ? 1 : 0);
}

static value n_file_seek(int argc, value *argv) {
    if (!need_args(argc, 3, "file_seek") || argv[0].kind != V_PTR) {
        g_set_error("file_seek(handle, offset, whence) needs a ptr");
        return v_int(-1);
    }
    return v_int(fseek((FILE*)argv[0].as.ptr,
                       (long)to_int(&argv[1]), (int)to_int(&argv[2])));
}

static value n_file_tell(int argc, value *argv) {
    if (!need_args(argc, 1, "file_tell") || argv[0].kind != V_PTR) {
        g_set_error("file_tell(handle) needs a ptr");
        return v_int(-1);
    }
    return v_int(ftell((FILE*)argv[0].as.ptr));
}

static value n_file_flush(int argc, value *argv) {
    if (!need_args(argc, 1, "file_flush") || argv[0].kind != V_PTR) {
        g_set_error("file_flush(handle) needs a ptr");
        return v_int(-1);
    }
    return v_int(fflush((FILE*)argv[0].as.ptr));
}

static value n_file_size(int argc, value *argv) {
    if (!need_args(argc, 1, "file_size") || argv[0].kind != V_STRING) {
        g_set_error("file_size(path) needs a string");
        return v_int(-1);
    }
    struct stat st;
    if (stat(argv[0].as.s, &st) != 0) return v_int(-1);
    return v_int((int64_t)st.st_size);
}

static value n_file_exists(int argc, value *argv) {
    if (!need_args(argc, 1, "file_exists") || argv[0].kind != V_STRING) {
        g_set_error("file_exists(path) needs a string");
        return v_int(0);
    }
    struct stat st;
    return v_int(stat(argv[0].as.s, &st) == 0 ? 1 : 0);
}

static value n_file_is_dir(int argc, value *argv) {
    if (!need_args(argc, 1, "file_is_dir") || argv[0].kind != V_STRING) {
        g_set_error("file_is_dir(path) needs a string");
        return v_int(0);
    }
    struct stat st;
    if (stat(argv[0].as.s, &st) != 0) return v_int(0);
    return v_int(S_ISDIR(st.st_mode) ? 1 : 0);
}

static value n_file_stat(int argc, value *argv) {
    if (!need_args(argc, 1, "file_stat") || argv[0].kind != V_STRING) {
        g_set_error("file_stat(path) needs a string");
        return v_nil();
    }
    struct stat st;
    if (stat(argv[0].as.s, &st) != 0) {
        g_set_error("file_stat: %s", strerror(errno));
        return v_nil();
    }
    value d = v_dict_value();
    dict_set(d.as.dict, "size",  v_int((int64_t)st.st_size));
    dict_set(d.as.dict, "mode",  v_int((int64_t)st.st_mode));
    dict_set(d.as.dict, "is_dir", v_int(S_ISDIR(st.st_mode) ? 1 : 0));
    dict_set(d.as.dict, "is_file", v_int(S_ISREG(st.st_mode) ? 1 : 0));
    dict_set(d.as.dict, "mtime", v_int((int64_t)st.st_mtime));
    dict_set(d.as.dict, "atime", v_int((int64_t)st.st_atime));
    dict_set(d.as.dict, "ctime", v_int((int64_t)st.st_ctime));
    dict_set(d.as.dict, "uid",   v_int((int64_t)st.st_uid));
    dict_set(d.as.dict, "gid",   v_int((int64_t)st.st_gid));
    return d;
}

static value n_file_mkdir(int argc, value *argv) {
    if (!need_args(argc, 1, "file_mkdir") || argv[0].kind != V_STRING) {
        g_set_error("file_mkdir(path) needs a string");
        return v_int(-1);
    }
    return v_int(mkdir(argv[0].as.s, 0755));
}

static value n_file_rmdir(int argc, value *argv) {
    if (!need_args(argc, 1, "file_rmdir") || argv[0].kind != V_STRING) {
        g_set_error("file_rmdir(path) needs a string");
        return v_int(-1);
    }
    return v_int(rmdir(argv[0].as.s));
}

static value n_file_unlink(int argc, value *argv) {
    if (!need_args(argc, 1, "file_unlink") || argv[0].kind != V_STRING) {
        g_set_error("file_unlink(path) needs a string");
        return v_int(-1);
    }
    return v_int(unlink(argv[0].as.s));
}

static value n_file_rename(int argc, value *argv) {
    if (!need_args(argc, 2, "file_rename") ||
        argv[0].kind != V_STRING || argv[1].kind != V_STRING) {
        g_set_error("file_rename(old, new) needs two strings");
        return v_int(-1);
    }
    return v_int(rename(argv[0].as.s, argv[1].as.s));
}

static value n_file_list(int argc, value *argv) {
    if (!need_args(argc, 1, "file_list") || argv[0].kind != V_STRING) {
        g_set_error("file_list(dir) needs a string");
        return v_arr();
    }
    DIR *dir = opendir(argv[0].as.s);
    if (!dir) {
        g_set_error("file_list: %s", strerror(errno));
        return v_arr();
    }
    value arr = v_arr();
    varr *a = arr.as.arr;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 16;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v_str(e->d_name);
    }
    closedir(dir);
    return arr;
}

static value n_file_read_file(int argc, value *argv) {
    if (!need_args(argc, 1, "file_read_file") || argv[0].kind != V_STRING) {
        g_set_error("file_read_file(path) needs a string");
        return v_str("");
    }
    FILE *f = fopen(argv[0].as.s, "rb");
    if (!f) {
        g_set_error("file_read_file: %s: %s", argv[0].as.s, strerror(errno));
        return v_str("");
    }
    sbuf sb; sbuf_init(&sb);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sbuf_putn(&sb, buf, n);
    }
    fclose(f);
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_file_write_file(int argc, value *argv) {
    if (!need_args(argc, 2, "file_write_file") ||
        argv[0].kind != V_STRING || argv[1].kind != V_STRING) {
        g_set_error("file_write_file(path, content) needs two strings");
        return v_int(-1);
    }
    FILE *f = fopen(argv[0].as.s, "wb");
    if (!f) {
        g_set_error("file_write_file: %s: %s", argv[0].as.s, strerror(errno));
        return v_int(-1);
    }
    size_t len = strlen(argv[1].as.s);
    size_t n = fwrite(argv[1].as.s, 1, len, f);
    fclose(f);
    return v_int((int64_t)n);
}

/* ================================================================== */
/* MATH                                                               */
/* ================================================================== */

static value math_unary(double (*fn)(double)) {
    /* we return a closure-like value via a static trampoline trick.
     * Since C doesn't have closures, we use a getter that returns the
     * actual native_fn. Each math function is its own native_fn. */
    (void)fn;
    return v_nil();
}
/* The above is a placeholder — we actually define each as a separate
 * function below. Repetitive but simple. */

#define MATH_UNARY(name, expr) \
    static value n_math_##name(int argc, value *argv) { \
        if (!need_args(argc, 1, "math_" #name)) return v_float(0); \
        double x = to_double(&argv[0]); \
        double r = (expr); \
        return v_float(r); \
    }

MATH_UNARY(sin,   sin(x))
MATH_UNARY(cos,   cos(x))
MATH_UNARY(tan,   tan(x))
MATH_UNARY(asin,  asin(x))
MATH_UNARY(acos,  acos(x))
MATH_UNARY(atan,  atan(x))
MATH_UNARY(sqrt,  sqrt(x))
MATH_UNARY(cbrt,  cbrt(x))
MATH_UNARY(log,   log(x))
MATH_UNARY(log2,  log2(x))
MATH_UNARY(log10, log10(x))
MATH_UNARY(exp,   exp(x))
MATH_UNARY(floor, floor(x))
MATH_UNARY(ceil,  ceil(x))
MATH_UNARY(round, round(x))
MATH_UNARY(abs,   fabs(x))
MATH_UNARY(sign,  (x > 0) ? 1.0 : (x < 0 ? -1.0 : 0.0))

static value n_math_atan2(int argc, value *argv) {
    if (!need_args(argc, 2, "math_atan2")) return v_float(0);
    return v_float(atan2(to_double(&argv[0]), to_double(&argv[1])));
}

static value n_math_pow(int argc, value *argv) {
    if (!need_args(argc, 2, "math_pow")) return v_float(0);
    return v_float(pow(to_double(&argv[0]), to_double(&argv[1])));
}

static value n_math_min(int argc, value *argv) {
    if (!need_args(argc, 2, "math_min")) return v_int(0);
    double a = to_double(&argv[0]);
    double b = to_double(&argv[1]);
    /* preserve int type if both are ints */
    if (argv[0].kind == V_INT && argv[1].kind == V_INT) {
        return v_int(a < b ? (int64_t)a : (int64_t)b);
    }
    return v_float(a < b ? a : b);
}

static value n_math_max(int argc, value *argv) {
    if (!need_args(argc, 2, "math_max")) return v_int(0);
    double a = to_double(&argv[0]);
    double b = to_double(&argv[1]);
    if (argv[0].kind == V_INT && argv[1].kind == V_INT) {
        return v_int(a > b ? (int64_t)a : (int64_t)b);
    }
    return v_float(a > b ? a : b);
}

static value n_math_clamp(int argc, value *argv) {
    if (!need_args(argc, 3, "math_clamp")) return v_int(0);
    double x = to_double(&argv[0]);
    double lo = to_double(&argv[1]);
    double hi = to_double(&argv[2]);
    double r = x < lo ? lo : (x > hi ? hi : x);
    if (argv[0].kind == V_INT && argv[1].kind == V_INT && argv[2].kind == V_INT) {
        return v_int((int64_t)r);
    }
    return v_float(r);
}

static value n_math_random(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_float((double)rand() / (double)RAND_MAX);
}

static value n_math_random_int(int argc, value *argv) {
    if (!need_args(argc, 2, "math_random_int")) return v_int(0);
    int64_t lo = to_int(&argv[0]);
    int64_t hi = to_int(&argv[1]);
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    if (hi == lo) return v_int(lo);
    return v_int(lo + (int64_t)(rand() % (int)(hi - lo + 1)));
}

static value n_math_random_seed(int argc, value *argv) {
    if (!need_args(argc, 1, "math_random_seed")) return v_nil();
    srand((unsigned)to_int(&argv[0]));
    return v_nil();
}

/* ================================================================== */
/* TIME                                                               */
/* ================================================================== */

static value n_time_now(int argc, value *argv) {
    (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return v_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static value n_time_now_s(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int((int64_t)time(NULL));
}

static value n_time_now_ns(int argc, value *argv) {
    (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return v_int((int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec);
}

static value n_time_sleep(int argc, value *argv) {
    if (!need_args(argc, 1, "time_sleep")) return v_nil();
    double s = to_double(&argv[0]);
    if (s < 0) s = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    return v_nil();
}

static value n_time_sleep_ms(int argc, value *argv) {
    if (!need_args(argc, 1, "time_sleep_ms")) return v_nil();
    int64_t ms = to_int(&argv[0]);
    if (ms < 0) ms = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
    return v_nil();
}

static value n_time_sleep_ns(int argc, value *argv) {
    if (!need_args(argc, 1, "time_sleep_ns")) return v_nil();
    int64_t ns = to_int(&argv[0]);
    if (ns < 0) ns = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    nanosleep(&ts, NULL);
    return v_nil();
}

static value n_time_format(int argc, value *argv) {
    if (!need_args(argc, 2, "time_format") || argv[1].kind != V_STRING) {
        g_set_error("time_format(ts, fmt) needs a number and a string");
        return v_str("");
    }
    time_t t = (time_t)to_int(&argv[0]);
    struct tm *tm = localtime(&t);
    if (!tm) return v_str("");
    char buf[256];
    strftime(buf, sizeof buf, argv[1].as.s, tm);
    return v_str(buf);
}

#define TIME_FIELD(name, field) \
    static value n_time_##name(int argc, value *argv) { \
        if (!need_args(argc, 1, "time_" #name)) return v_int(0); \
        time_t t = (time_t)to_int(&argv[0]); \
        struct tm *tm = localtime(&t); \
        if (!tm) return v_int(0); \
        return v_int((int64_t)tm->field); \
    }

TIME_FIELD(year, tm_year + 1900)
TIME_FIELD(month, tm_mon + 1)
TIME_FIELD(day, tm_mday)
TIME_FIELD(hour, tm_hour)
TIME_FIELD(min, tm_min)
TIME_FIELD(sec, tm_sec)
TIME_FIELD(weekday, tm_wday)

/* ================================================================== */
/* PROCESS                                                            */
/* ================================================================== */

static value n_proc_getpid(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int((int64_t)getpid());
}

static value n_proc_getppid(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int((int64_t)getppid());
}

static value n_proc_env(int argc, value *argv) {
    if (!need_args(argc, 1, "proc_env") || argv[0].kind != V_STRING) {
        g_set_error("proc_env(name) needs a string");
        return v_nil();
    }
    const char *v = getenv(argv[0].as.s);
    if (!v) return v_nil();
    return v_str(v);
}

static value n_proc_env_set(int argc, value *argv) {
    if (!need_args(argc, 2, "proc_env_set") ||
        argv[0].kind != V_STRING || argv[1].kind != V_STRING) {
        g_set_error("proc_env_set(name, val) needs two strings");
        return v_int(-1);
    }
    return v_int(setenv(argv[0].as.s, argv[1].as.s, 1));
}

static value n_proc_env_unset(int argc, value *argv) {
    if (!need_args(argc, 1, "proc_env_unset") || argv[0].kind != V_STRING) {
        g_set_error("proc_env_unset(name) needs a string");
        return v_int(-1);
    }
    return v_int(unsetenv(argv[0].as.s));
}

extern char **environ;

static value n_proc_env_list(int argc, value *argv) {
    (void)argc; (void)argv;
    value arr = v_arr();
    varr *a = arr.as.arr;
    for (char **e = environ; *e; e++) {
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 16;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v_str(*e);
    }
    return arr;
}

static value n_proc_cwd(int argc, value *argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) {
        g_set_error("proc_cwd: %s", strerror(errno));
        return v_str("");
    }
    return v_str(buf);
}

static value n_proc_chdir(int argc, value *argv) {
    if (!need_args(argc, 1, "proc_chdir") || argv[0].kind != V_STRING) {
        g_set_error("proc_chdir(path) needs a string");
        return v_int(-1);
    }
    return v_int(chdir(argv[0].as.s));
}

static value n_proc_fork(int argc, value *argv) {
    (void)argc; (void)argv;
    return v_int((int64_t)fork());
}

static value n_proc_wait(int argc, value *argv) {
    if (!need_args(argc, 1, "proc_wait")) return v_int(-1);
    int status;
    pid_t pid = (pid_t)to_int(&argv[0]);
    waitpid(pid, &status, 0);
    return v_int(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static value n_proc_wait_any(int argc, value *argv) {
    (void)argc; (void)argv;
    int status;
    pid_t pid = wait(&status);
    value arr = v_arr();
    varr *a = arr.as.arr;
    a->cap = 2;
    a->items = realloc(a->items, a->cap * sizeof(value));
    a->items[a->len++] = v_int((int64_t)pid);
    a->items[a->len++] = v_int(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return arr;
}

static value n_proc_kill(int argc, value *argv) {
    if (!need_args(argc, 2, "proc_kill")) return v_int(-1);
    return v_int(kill((pid_t)to_int(&argv[0]), (int)to_int(&argv[1])));
}

/* ================================================================== */
/* FUNCTIONAL                                                         */
/* ================================================================== */

static value n_call(int argc, value *argv) {
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_ARRAY) {
        g_set_error("call(squire_name, args_array) needs a string and an array");
        return v_nil();
    }
    varr *a = argv[1].as.arr;
    return interp_call_global_squire(argv[0].as.s, (int)a->len, a->items);
}

static value n_apply(int argc, value *argv) {
    return n_call(argc, argv);
}

/* ================================================================== */
/* REGEX — POSIX extended regex                                       */
/* ================================================================== */

/* re_match(str, pattern) -> int      1 if full match, 0 if not
 * re_find(str, pattern) -> array     array of [match, start, end] or nil
 * re_find_all(str, pattern) -> array  array of all matches
 * re_replace(str, pattern, repl) -> string  replace first match
 * re_replace_all(str, pattern, repl) -> string  replace all matches
 * re_split(str, pattern) -> array    split on pattern
 * re_groups(str, pattern) -> array   array of capture groups (or nil)
 */

static value n_re_match(int argc, value *argv) {
    if (!need_args(argc, 2, "re_match")) return v_int(0);
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) {
        g_set_error("re_match: invalid regex '%s'", pat);
        return v_int(0);
    }
    int rc = regexec(&re, str, 0, NULL, 0);
    regfree(&re);
    return v_int(rc == 0 ? 1 : 0);
}

static value n_re_find(int argc, value *argv) {
    if (!need_args(argc, 2, "re_find")) return v_nil();
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        g_set_error("re_find: invalid regex '%s'", pat);
        return v_nil();
    }
    regmatch_t match;
    if (regexec(&re, str, 1, &match, 0) != 0) {
        regfree(&re);
        return v_nil();
    }
    value arr = v_arr();
    varr *a = arr.as.arr;
    a->cap = 3;
    a->items = realloc(a->items, a->cap * sizeof(value));
    /* the matched substring */
    int start = match.rm_so;
    int end = match.rm_eo;
    size_t len = (size_t)(end - start);
    char *buf = malloc(len + 1);
    memcpy(buf, str + start, len);
    buf[len] = '\0';
    a->items[a->len++] = v_str_take(buf);
    a->items[a->len++] = v_int((int64_t)start);
    a->items[a->len++] = v_int((int64_t)end);
    regfree(&re);
    return arr;
}

static value n_re_find_all(int argc, value *argv) {
    if (!need_args(argc, 2, "re_find_all")) return v_arr();
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        g_set_error("re_find_all: invalid regex '%s'", pat);
        return v_arr();
    }
    value arr = v_arr();
    varr *a = arr.as.arr;
    const char *p = str;
    while (*p) {
        regmatch_t match;
        if (regexec(&re, p, 1, &match, 0) != 0) break;
        int start = match.rm_so;
        int end = match.rm_eo;
        size_t len = (size_t)(end - start);
        char *buf = malloc(len + 1);
        memcpy(buf, p + start, len);
        buf[len] = '\0';
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v_str_take(buf);
        /* advance past match (or by 1 if empty match to avoid infinite loop) */
        p += end;
        if (end == 0) p++;
    }
    regfree(&re);
    return arr;
}

static value n_re_replace(int argc, value *argv) {
    if (!need_args(argc, 3, "re_replace")) return v_str("");
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    const char *repl = str_arg(&argv[2]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        g_set_error("re_replace: invalid regex '%s'", pat);
        return v_str("");
    }
    regmatch_t match;
    if (regexec(&re, str, 1, &match, 0) != 0) {
        regfree(&re);
        return v_str(str);
    }
    sbuf sb; sbuf_init(&sb);
    sbuf_putn(&sb, str, (size_t)match.rm_so);
    sbuf_puts(&sb, repl);
    sbuf_puts(&sb, str + match.rm_eo);
    regfree(&re);
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_re_replace_all(int argc, value *argv) {
    if (!need_args(argc, 3, "re_replace_all")) return v_str("");
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    const char *repl = str_arg(&argv[2]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        g_set_error("re_replace_all: invalid regex '%s'", pat);
        return v_str("");
    }
    sbuf sb; sbuf_init(&sb);
    const char *p = str;
    while (*p) {
        regmatch_t match;
        if (regexec(&re, p, 1, &match, 0) != 0) {
            sbuf_puts(&sb, p);
            break;
        }
        sbuf_putn(&sb, p, (size_t)match.rm_so);
        sbuf_puts(&sb, repl);
        p += match.rm_eo;
        if (match.rm_eo == 0) {
            sbuf_putc(&sb, *p);
            p++;
        }
    }
    regfree(&re);
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value n_re_split(int argc, value *argv) {
    if (!need_args(argc, 2, "re_split")) return v_arr();
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        g_set_error("re_split: invalid regex '%s'", pat);
        return v_arr();
    }
    value arr = v_arr();
    varr *a = arr.as.arr;
    const char *p = str;
    while (*p) {
        regmatch_t match;
        if (regexec(&re, p, 1, &match, 0) != 0) {
            /* rest of string */
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str(p);
            break;
        }
        if (match.rm_so > 0) {
            size_t len = (size_t)match.rm_so;
            char *buf = malloc(len + 1);
            memcpy(buf, p, len);
            buf[len] = '\0';
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str_take(buf);
        }
        p += match.rm_eo;
        if (match.rm_eo == 0) {
            /* empty match — add current char and advance */
            char buf[2] = { *p, 0 };
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str(buf);
            p++;
        }
        if (!*p) {
            /* string ended with delimiter — add empty */
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str("");
            break;
        }
    }
    regfree(&re);
    return arr;
}

static value n_re_groups(int argc, value *argv) {
    if (!need_args(argc, 2, "re_groups")) return v_nil();
    const char *str = str_arg(&argv[0]);
    const char *pat = str_arg(&argv[1]);
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        g_set_error("re_groups: invalid regex '%s'", pat);
        return v_nil();
    }
    /* We need to know how many groups the pattern has. regcomp doesn't
     * tell us directly, so we try with increasing nmatch until we stop
     * seeing new groups. Cap at 20 for safety. */
    int nmatch = 20;
    regmatch_t matches[20];
    if (regexec(&re, str, (size_t)nmatch, matches, 0) != 0) {
        regfree(&re);
        return v_nil();
    }
    value arr = v_arr();
    varr *a = arr.as.arr;
    /* Skip matches[0] (the full match) — groups start at 1 */
    for (int i = 1; i < nmatch; i++) {
        if (matches[i].rm_so < 0) break;  /* no more groups */
        size_t len = (size_t)(matches[i].rm_eo - matches[i].rm_so);
        char *buf = malloc(len + 1);
        memcpy(buf, str + matches[i].rm_so, len);
        buf[len] = '\0';
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v_str_take(buf);
    }
    regfree(&re);
    return arr;
}

/* ================================================================== */
/* TYPE CONVERSION                                                    */
/* ================================================================== */

static value n_to_str(int argc, value *argv) {
    if (!need_args(argc, 1, "to_str")) return v_str("");
    char *s = v_to_string(&argv[0]);
    value v = v_str(s);
    free(s);
    return v;
}

static value n_to_int(int argc, value *argv) {
    if (!need_args(argc, 1, "to_int")) return v_int(0);
    return v_int(to_int(&argv[0]));
}

static value n_to_float(int argc, value *argv) {
    if (!need_args(argc, 1, "to_float")) return v_float(0);
    return v_float(to_double(&argv[0]));
}

static value n_to_bool(int argc, value *argv) {
    if (!need_args(argc, 1, "to_bool")) return v_int(0);
    return v_truthy(&argv[0]);
}

static value n_to_array(int argc, value *argv) {
    if (!need_args(argc, 1, "to_array")) return v_arr();
    if (argv[0].kind == V_ARRAY) return v_clone(&argv[0]);
    /* Wrap single value in an array */
    value arr = v_arr();
    varr *a = arr.as.arr;
    a->cap = 1;
    a->items = realloc(a->items, sizeof(value));
    a->items[0] = v_clone(&argv[0]);
    a->len = 1;
    return arr;
}

static value n_to_dict(int argc, value *argv) {
    if (!need_args(argc, 1, "to_dict")) return v_dict_value();
    if (argv[0].kind == V_DICT) return v_clone(&argv[0]);
    g_set_error("to_dict: cannot convert %s to dict", "value");
    return v_dict_value();
}

/* ================================================================== */
/* MORE MATH — number theory and geometry                             */
/* ================================================================== */

static int64_t igcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}

static value n_math_gcd(int argc, value *argv) {
    if (!need_args(argc, 2, "math_gcd")) return v_int(0);
    return v_int(igcd(to_int(&argv[0]), to_int(&argv[1])));
}

static value n_math_lcm(int argc, value *argv) {
    if (!need_args(argc, 2, "math_lcm")) return v_int(0);
    int64_t a = to_int(&argv[0]);
    int64_t b = to_int(&argv[1]);
    if (a == 0 || b == 0) return v_int(0);
    int64_t g = igcd(a, b);
    return v_int((a / g) * b);
}

static value n_math_fact(int argc, value *argv) {
    if (!need_args(argc, 1, "math_fact")) return v_int(1);
    int64_t n = to_int(&argv[0]);
    if (n < 0) return v_int(1);
    if (n > 20) {
        g_set_error("math_fact: %ld! overflows int64", (long)n);
        return v_int(-1);
    }
    int64_t r = 1;
    for (int64_t i = 2; i <= n; i++) r *= i;
    return v_int(r);
}

static value n_math_is_prime(int argc, value *argv) {
    if (!need_args(argc, 1, "math_is_prime")) return v_int(0);
    int64_t n = to_int(&argv[0]);
    if (n < 2) return v_int(0);
    if (n < 4) return v_int(1);
    if (n % 2 == 0) return v_int(0);
    for (int64_t i = 3; i * i <= n; i += 2) {
        if (n % i == 0) return v_int(0);
    }
    return v_int(1);
}

static value n_math_hypot(int argc, value *argv) {
    if (!need_args(argc, 2, "math_hypot")) return v_float(0);
    return v_float(hypot(to_double(&argv[0]), to_double(&argv[1])));
}

static value n_math_deg2rad(int argc, value *argv) {
    if (!need_args(argc, 1, "math_deg2rad")) return v_float(0);
    return v_float(to_double(&argv[0]) * 3.14159265358979323846 / 180.0);
}

static value n_math_rad2deg(int argc, value *argv) {
    if (!need_args(argc, 1, "math_rad2deg")) return v_float(0);
    return v_float(to_double(&argv[0]) * 180.0 / 3.14159265358979323846);
}

static value n_math_comb(int argc, value *argv) {
    if (!need_args(argc, 2, "math_comb")) return v_int(0);
    int64_t n = to_int(&argv[0]);
    int64_t k = to_int(&argv[1]);
    if (k < 0 || k > n) return v_int(0);
    if (k == 0 || k == n) return v_int(1);
    if (k > n - k) k = n - k;  /* use smaller k */
    int64_t r = 1;
    for (int64_t i = 0; i < k; i++) {
        r = r * (n - i) / (i + 1);
    }
    return v_int(r);
}

static value n_math_perm(int argc, value *argv) {
    if (!need_args(argc, 2, "math_perm")) return v_int(0);
    int64_t n = to_int(&argv[0]);
    int64_t k = to_int(&argv[1]);
    if (k < 0 || k > n) return v_int(0);
    int64_t r = 1;
    for (int64_t i = 0; i < k; i++) r *= (n - i);
    return v_int(r);
}

/* ================================================================== */
/* MORE STRINGS — padding, counting, charset ops                      */
/* ================================================================== */

static value n_str_pad_left(int argc, value *argv) {
    if (!need_args(argc, 3, "str_pad_left")) return v_str("");
    const char *s = str_arg(&argv[0]);
    int64_t width = to_int(&argv[1]);
    const char *pad = str_arg(&argv[2]);
    size_t slen = strlen(s);
    if ((int64_t)slen >= width || !pad[0]) return v_str(s);
    size_t padlen = strlen(pad);
    size_t total_pad = (size_t)(width - (int64_t)slen);
    char *buf = malloc((size_t)width + 1);
    size_t pos = 0;
    while (pos < total_pad) {
        buf[pos] = pad[pos % padlen];
        pos++;
    }
    memcpy(buf + pos, s, slen);
    buf[pos + slen] = '\0';
    return v_str_take(buf);
}

static value n_str_pad_right(int argc, value *argv) {
    if (!need_args(argc, 3, "str_pad_right")) return v_str("");
    const char *s = str_arg(&argv[0]);
    int64_t width = to_int(&argv[1]);
    const char *pad = str_arg(&argv[2]);
    size_t slen = strlen(s);
    if ((int64_t)slen >= width || !pad[0]) return v_str(s);
    size_t padlen = strlen(pad);
    char *buf = malloc((size_t)width + 1);
    memcpy(buf, s, slen);
    size_t pos = slen;
    while (pos < (size_t)width) {
        buf[pos] = pad[(pos - slen) % padlen];
        pos++;
    }
    buf[pos] = '\0';
    return v_str_take(buf);
}

static value n_str_center(int argc, value *argv) {
    if (!need_args(argc, 3, "str_center")) return v_str("");
    const char *s = str_arg(&argv[0]);
    int64_t width = to_int(&argv[1]);
    const char *pad = str_arg(&argv[2]);
    size_t slen = strlen(s);
    if ((int64_t)slen >= width || !pad[0]) return v_str(s);
    size_t total_pad = (size_t)width - slen;
    size_t left = total_pad / 2;
    size_t right = total_pad - left;
    size_t padlen = strlen(pad);
    char *buf = malloc((size_t)width + 1);
    size_t pos = 0;
    for (size_t i = 0; i < left; i++) { buf[pos++] = pad[i % padlen]; }
    memcpy(buf + pos, s, slen); pos += slen;
    for (size_t i = 0; i < right; i++) { buf[pos++] = pad[i % padlen]; }
    buf[pos] = '\0';
    return v_str_take(buf);
}

static value n_str_count(int argc, value *argv) {
    if (!need_args(argc, 2, "str_count")) return v_int(0);
    const char *s = str_arg(&argv[0]);
    const char *sub = str_arg(&argv[1]);
    size_t slen = strlen(sub);
    if (slen == 0) return v_int(0);
    int64_t count = 0;
    const char *p = s;
    while ((p = strstr(p, sub)) != NULL) {
        count++;
        p += slen;
    }
    return v_int(count);
}

/* ================================================================== */
/* GETTERS — expose each native_fn to interp.c                        */
/* ================================================================== */

#define GETTER(name) native_fn stdlib_##name(void) { return n_##name; }

GETTER(str_find)         GETTER(str_find_from)    GETTER(str_slice)
GETTER(str_split)        GETTER(str_join)         GETTER(str_replace)
GETTER(str_replace_all)  GETTER(str_trim)         GETTER(str_trim_left)
GETTER(str_trim_right)   GETTER(str_upper)        GETTER(str_lower)
GETTER(str_starts_with)  GETTER(str_ends_with)    GETTER(str_contains)
GETTER(str_repeat)       GETTER(str_reverse)      GETTER(str_chars)
GETTER(str_bytes)        GETTER(str_from_bytes)   GETTER(str_to_int)
GETTER(str_to_float)     GETTER(int_to_str)       GETTER(float_to_str)
GETTER(str_format)

GETTER(arr_push)    GETTER(arr_pop)     GETTER(arr_shift)   GETTER(arr_unshift)
GETTER(arr_map)     GETTER(arr_filter)  GETTER(arr_reduce)  GETTER(arr_sort)
GETTER(arr_reverse) GETTER(arr_concat)  GETTER(arr_slice)   GETTER(arr_find)
GETTER(arr_contains)

GETTER(dict_new)  GETTER(dict_set)  GETTER(dict_get)  GETTER(dict_get_or)
GETTER(dict_has)  GETTER(dict_del)   GETTER(dict_keys)  GETTER(dict_vals)
GETTER(dict_size) GETTER(dict_clear)

GETTER(file_open)     GETTER(file_close)    GETTER(file_read)     GETTER(file_readln)
GETTER(file_read_all) GETTER(file_write)    GETTER(file_writeln)  GETTER(file_eof)
GETTER(file_seek)     GETTER(file_tell)     GETTER(file_flush)    GETTER(file_size)
GETTER(file_exists)   GETTER(file_is_dir)   GETTER(file_stat)     GETTER(file_mkdir)
GETTER(file_rmdir)    GETTER(file_unlink)   GETTER(file_rename)   GETTER(file_list)
GETTER(file_read_file) GETTER(file_write_file)

GETTER(math_sin)  GETTER(math_cos)  GETTER(math_tan)  GETTER(math_asin)
GETTER(math_acos) GETTER(math_atan) GETTER(math_atan2) GETTER(math_pow)
GETTER(math_sqrt) GETTER(math_cbrt)
GETTER(math_log)  GETTER(math_log2) GETTER(math_log10) GETTER(math_exp)
GETTER(math_floor) GETTER(math_ceil) GETTER(math_round)
GETTER(math_min)  GETTER(math_max)  GETTER(math_abs)  GETTER(math_sign)
GETTER(math_clamp) GETTER(math_random) GETTER(math_random_int) GETTER(math_random_seed)

GETTER(time_now)  GETTER(time_now_s)  GETTER(time_now_ns)
GETTER(time_sleep) GETTER(time_sleep_ms) GETTER(time_sleep_ns)
GETTER(time_format)
GETTER(time_year) GETTER(time_month) GETTER(time_day)
GETTER(time_hour) GETTER(time_min) GETTER(time_sec) GETTER(time_weekday)

GETTER(proc_getpid) GETTER(proc_getppid)
GETTER(proc_env) GETTER(proc_env_set) GETTER(proc_env_unset) GETTER(proc_env_list)
GETTER(proc_cwd) GETTER(proc_chdir)
GETTER(proc_fork) GETTER(proc_wait) GETTER(proc_wait_any) GETTER(proc_kill)

GETTER(call) GETTER(apply)

/* Regex */
GETTER(re_match) GETTER(re_find) GETTER(re_find_all)
GETTER(re_replace) GETTER(re_replace_all)
GETTER(re_split) GETTER(re_groups)

/* Type conversion */
GETTER(to_str) GETTER(to_int) GETTER(to_float)
GETTER(to_bool) GETTER(to_array) GETTER(to_dict)

/* More math */
GETTER(math_gcd) GETTER(math_lcm) GETTER(math_fact)
GETTER(math_is_prime) GETTER(math_hypot)
GETTER(math_deg2rad) GETTER(math_rad2deg)
GETTER(math_comb) GETTER(math_perm)

/* More strings */
GETTER(str_pad_left) GETTER(str_pad_right)
GETTER(str_center) GETTER(str_count)
