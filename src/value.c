/* value.c — runtime value representation.
 *
 * Values are tagged unions. Strings are owned (malloc'd), arrays are
 * heap-allocated with refcounting. Function values point at AST nodes
 * plus their closure environment. Native functions are C function pointers.
 */

#include "glyph.h"
#include "platform.h"

value v_int(int64_t i) {
    value v = {0};
    v.kind = V_INT;
    v.as.i = i;
    return v;
}
value v_float(double f) {
    value v = {0};
    v.kind = V_FLOAT;
    v.as.f = f;
    return v;
}
value v_str(const char *s) {
    value v = {0};
    v.kind = V_STRING;
    v.as.s = strdup(s ? s : "");
    return v;
}
value v_str_take(char *s) {
    value v = {0};
    v.kind = V_STRING;
    v.as.s = s;
    return v;
}
value v_nil(void) {
    value v = {0};
    v.kind = V_NIL;
    return v;
}
value v_native(native_fn fn) {
    value v = {0};
    v.kind = V_NATIVE;
    v.as.nat = fn;
    return v;
}
value v_ptr(void *p) {
    value v = {0};
    v.kind = V_PTR;
    v.as.ptr = p;
    return v;
}

/* arrays: heap-allocated varr struct (defined in glyph.h) */
static varr *arr_new(void) {
    varr *a = calloc(1, sizeof(varr));
    return a;
}

value v_arr(void) {
    value v = {0};
    v.kind = V_ARRAY;
    v.as.arr = arr_new();
    return v;
}

/* ------------------------------------------------------------------ */
/* dict (hash table) — FNV-1a + linear probing                        */
/* ------------------------------------------------------------------ */

#define DICT_INITIAL_CAP 8

static uint64_t dict_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

vdict *dict_new(void) {
    vdict *d = calloc(1, sizeof(vdict));
    d->cap = DICT_INITIAL_CAP;
    d->keys = calloc(d->cap, sizeof(char*));
    d->vals = calloc(d->cap, sizeof(value));
    return d;
}

void dict_free(vdict *d) {
    if (!d) return;
    for (size_t i = 0; i < d->cap; i++) {
        if (d->keys[i]) {
            free(d->keys[i]);
            v_free(&d->vals[i]);
        }
    }
    free(d->keys);
    free(d->vals);
    free(d);
}

static void dict_grow(vdict *d) {
    size_t old_cap = d->cap;
    char **old_keys = d->keys;
    value *old_vals = d->vals;
    d->cap *= 2;
    d->keys = calloc(d->cap, sizeof(char*));
    d->vals = calloc(d->cap, sizeof(value));
    d->len = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old_keys[i]) {
            dict_set(d, old_keys[i], old_vals[i]);
            free(old_keys[i]);
        }
    }
    free(old_keys);
    free(old_vals);
}

int dict_set(vdict *d, const char *key, value v) {
    if (!d || !key) return -1;
    if ((d->len + 1) * 4 > d->cap * 3) dict_grow(d);
    size_t mask = d->cap - 1;
    size_t i = (size_t)dict_hash(key) & mask;
    while (d->keys[i]) {
        if (strcmp(d->keys[i], key) == 0) {
            v_free(&d->vals[i]);
            d->vals[i] = v;
            return 0;
        }
        i = (i + 1) & mask;
    }
    d->keys[i] = strdup(key);
    d->vals[i] = v;
    d->len++;
    return 0;
}

value dict_get(vdict *d, const char *key, int *found) {
    if (found) *found = 0;
    if (!d || !key) return v_nil();
    size_t mask = d->cap - 1;
    size_t i = (size_t)dict_hash(key) & mask;
    while (d->keys[i]) {
        if (strcmp(d->keys[i], key) == 0) {
            if (found) *found = 1;
            return d->vals[i];
        }
        i = (i + 1) & mask;
    }
    return v_nil();
}

int dict_has(vdict *d, const char *key) {
    int found;
    dict_get(d, key, &found);
    return found;
}

int dict_del(vdict *d, const char *key) {
    if (!d || !key) return 0;
    size_t mask = d->cap - 1;
    size_t i = (size_t)dict_hash(key) & mask;
    while (d->keys[i]) {
        if (strcmp(d->keys[i], key) == 0) {
            free(d->keys[i]);
            v_free(&d->vals[i]);
            d->keys[i] = NULL;
            d->len--;
            size_t j = (i + 1) & mask;
            while (d->keys[j]) {
                char *k = d->keys[j];
                value v = d->vals[j];
                d->keys[j] = NULL;
                d->vals[j] = v_nil();
                d->len--;
                dict_set(d, k, v);
                free(k);
                j = (j + 1) & mask;
            }
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

value v_dict_value(void) {
    value v = {0};
    v.kind = V_DICT;
    v.as.dict = dict_new();
    return v;
}

void v_free(value *v) {
    if (!v) return;
    switch (v->kind) {
        case V_STRING:
            free(v->as.s);
            v->as.s = NULL;
            break;
        case V_ARRAY:
            break;
        case V_DICT:
            break;
        default: break;
    }
}

value v_clone(const value *v) {
    /* returns a value with the same payload; for strings we share the pointer
       (caller must NOT free if shared). For our interpreter, we never clone
       strings — we always duplicate when assigning. */
    value out = *v;
    if (v->kind == V_STRING && v->as.s) out.as.s = strdup(v->as.s);
    return out;
}

value v_truthy(const value *v) {
    value r = v_int(0);
    switch (v->kind) {
        case V_INT:    r.as.i = (v->as.i != 0); break;
        case V_FLOAT:  r.as.i = (v->as.f != 0.0); break;
        case V_STRING: r.as.i = (v->as.s && v->as.s[0] != '\0'); break;
        case V_NIL:    r.as.i = 0; break;
        case V_ARRAY:  r.as.i = (v->as.arr && v->as.arr->len > 0); break;
        case V_DICT:   r.as.i = (v->as.dict && v->as.dict->len > 0); break;
        case V_FUNC:
        case V_NATIVE: r.as.i = 1; break;
        case V_PTR:    r.as.i = (v->as.ptr != NULL); break;
    }
    return r;
}

int v_eq(const value *a, const value *b) {
    if (a->kind != b->kind) {
        /* int/float compare */
        if ((a->kind == V_INT && b->kind == V_FLOAT) ||
            (a->kind == V_FLOAT && b->kind == V_INT)) {
            double av = a->kind == V_INT ? (double)a->as.i : a->as.f;
            double bv = b->kind == V_INT ? (double)b->as.i : b->as.f;
            return av == bv;
        }
        /* Mixed string/int: single-char string == int compares char code */
        if (a->kind == V_STRING && strlen(a->as.s) == 1 &&
            (b->kind == V_INT || b->kind == V_FLOAT)) {
            double av = (double)(unsigned char)a->as.s[0];
            double bv = b->kind == V_INT ? (double)b->as.i : b->as.f;
            return av == bv;
        }
        if (b->kind == V_STRING && strlen(b->as.s) == 1 &&
            (a->kind == V_INT || a->kind == V_FLOAT)) {
            double av = a->kind == V_INT ? (double)a->as.i : a->as.f;
            double bv = (double)(unsigned char)b->as.s[0];
            return av == bv;
        }
        return 0;
    }
    switch (a->kind) {
        case V_INT:    return a->as.i == b->as.i;
        case V_FLOAT:  return a->as.f == b->as.f;
        case V_STRING: return strcmp(a->as.s, b->as.s) == 0;
        case V_NIL:    return 1;
        case V_ARRAY:  return a->as.arr == b->as.arr;
        case V_DICT:   return a->as.dict == b->as.dict;
        case V_PTR:    return a->as.ptr == b->as.ptr;
        default:       return 0;
    }
}

int v_lt(const value *a, const value *b) {
    if (a->kind == V_INT && b->kind == V_INT) return a->as.i < b->as.i;
    if (a->kind == V_FLOAT && b->kind == V_FLOAT) return a->as.f < b->as.f;
    if ((a->kind == V_INT || a->kind == V_FLOAT) && (b->kind == V_INT || b->kind == V_FLOAT)) {
        double av = a->kind == V_INT ? (double)a->as.i : a->as.f;
        double bv = b->kind == V_INT ? (double)b->as.i : b->as.f;
        return av < bv;
    }
    if (a->kind == V_STRING && b->kind == V_STRING) return strcmp(a->as.s, b->as.s) < 0;
    /* Mixed string/int comparison: treat single-char strings as their char code.
     * This makes src[i] >= 97 work for character classification (lexer use case). */
    if (a->kind == V_STRING && strlen(a->as.s) == 1 && (b->kind == V_INT || b->kind == V_FLOAT)) {
        double av = (double)(unsigned char)a->as.s[0];
        double bv = b->kind == V_INT ? (double)b->as.i : b->as.f;
        return av < bv;
    }
    if (b->kind == V_STRING && strlen(b->as.s) == 1 && (a->kind == V_INT || a->kind == V_FLOAT)) {
        double av = a->kind == V_INT ? (double)a->as.i : a->as.f;
        double bv = (double)(unsigned char)b->as.s[0];
        return av < bv;
    }
    g_set_error("cannot compare %d with %d", a->kind, b->kind);
    return 0;
}

char *v_to_string(const value *v) {
    sbuf sb; sbuf_init(&sb);
    switch (v->kind) {
        case V_INT:    sbuf_printf(&sb, "%ld", (long)v->as.i); break;
        case V_FLOAT:  {
            /* print floats that are whole numbers with a trailing .0 */
            double ip;
            if (fabs(v->as.f) > 1e15 || isnan(v->as.f) || isinf(v->as.f)) {
                sbuf_printf(&sb, "%g", v->as.f);
            } else if (modf(v->as.f, &ip) == 0.0) {
                sbuf_printf(&sb, "%.1f", v->as.f);
            } else {
                sbuf_printf(&sb, "%g", v->as.f);
            }
            break;
        }
        case V_STRING: sbuf_puts(&sb, v->as.s ? v->as.s : ""); break;
        case V_NIL:    sbuf_puts(&sb, "nil"); break;
        case V_ARRAY: {
            sbuf_putc(&sb, '[');
            for (size_t i = 0; i < v->as.arr->len; i++) {
                if (i) sbuf_puts(&sb, ", ");
                char *es = v_to_string(&v->as.arr->items[i]);
                /* quote strings inside arrays */
                if (v->as.arr->items[i].kind == V_STRING) {
                    sbuf_putc(&sb, '"');
                    sbuf_puts(&sb, es);
                    sbuf_putc(&sb, '"');
                } else {
                    sbuf_puts(&sb, es);
                }
                free(es);
            }
            sbuf_putc(&sb, ']');
            break;
        }
        case V_FUNC:   sbuf_printf(&sb, "<squire>"); break;
        case V_NATIVE: sbuf_printf(&sb, "<native>"); break;
        case V_PTR:    sbuf_printf(&sb, "0x%lx", (unsigned long)v->as.ptr); break;
        case V_DICT: {
            sbuf_puts(&sb, "{");
            int first = 1;
            for (size_t i = 0; i < v->as.dict->cap; i++) {
                if (v->as.dict->keys[i]) {
                    if (!first) sbuf_puts(&sb, ", ");
                    first = 0;
                    sbuf_putc(&sb, '"');
                    sbuf_puts(&sb, v->as.dict->keys[i]);
                    sbuf_puts(&sb, "\": ");
                    char *es = v_to_string(&v->as.dict->vals[i]);
                    if (v->as.dict->vals[i].kind == V_STRING) {
                        sbuf_putc(&sb, '"');
                        sbuf_puts(&sb, es);
                        sbuf_putc(&sb, '"');
                    } else {
                        sbuf_puts(&sb, es);
                    }
                    free(es);
                }
            }
            sbuf_putc(&sb, '}');
            break;
        }
    }
    if (!sb.data) sbuf_puts(&sb, "");
    return sb.data;
}

/* ------------------------------------------------------------------ */
/* environment                                                        */
/* ------------------------------------------------------------------ */

env *env_new(env *parent) {
    env *e = calloc(1, sizeof(env));
    e->parent = parent;
    return e;
}

void env_free(env *e) {
    if (!e) return;
    for (size_t i = 0; i < e->len; i++) {
        free(e->names[i]);
        v_free(&e->vals[i]);
    }
    free(e->names);
    free(e->vals);
    free(e);
}

value env_get(env *e, const char *name, int *found) {
    *found = 0;
    for (env *cur = e; cur; cur = cur->parent) {
        for (size_t i = 0; i < cur->len; i++) {
            if (strcmp(cur->names[i], name) == 0) {
                *found = 1;
                return cur->vals[i];
            }
        }
    }
    return v_nil();
}

int env_set(env *e, const char *name, value v) {
    /* walk up to find an existing binding */
    for (env *c = e; c; c = c->parent) {
        for (size_t i = 0; i < c->len; i++) {
            if (strcmp(c->names[i], name) == 0) {
                v_free(&c->vals[i]);
                c->vals[i] = v;
                return 0;
            }
        }
    }
    /* not found: create in current scope */
    return env_set_local(e, name, v);
}

int env_set_local(env *e, const char *name, value v) {
    /* if exists locally, overwrite; else append */
    for (size_t i = 0; i < e->len; i++) {
        if (strcmp(e->names[i], name) == 0) {
            v_free(&e->vals[i]);
            e->vals[i] = v;
            return 0;
        }
    }
    if (e->len + 1 > e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->names = realloc(e->names, e->cap * sizeof(char*));
        e->vals  = realloc(e->vals,  e->cap * sizeof(value));
    }
    e->names[e->len] = strdup(name);
    e->vals[e->len]  = v;
    e->len++;
    return 0;
}
