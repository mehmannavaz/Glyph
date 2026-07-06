/* json.c — JSON parser and serializer for Glyph.
 *
 * Two public functions used by the JSON builtins:
 *   json_parse_value(src, &out)  — parse JSON text into a Glyph value
 *   json_stringify_value(v, pretty, indent) — serialize Glyph value to JSON
 *
 * Supported types:
 *   JSON number  <-> V_INT or V_FLOAT
 *   JSON string  <-> V_STRING
 *   JSON bool    <-> V_INT (1 or 0)
 *   JSON null    <-> V_NIL
 *   JSON array   <-> V_ARRAY
 *   JSON object  <-> V_DICT
 *
 * The parser is a small recursive descent parser. It's not the fastest
 * JSON parser in the world, but it's correct and small.
 */
#include "glyph.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ================================================================== */
/* Parser                                                             */
/* ================================================================== */

typedef struct {
    const char *p;
    const char *end;
    int err;
    char errmsg[256];
} jparser;

static void j_error(jparser *J, const char *fmt, ...) {
    J->err = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(J->errmsg, sizeof J->errmsg, fmt, ap);
    va_end(ap);
}

static void j_skip_ws(jparser *J) {
    while (J->p < J->end) {
        char c = *J->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') J->p++;
        else break;
    }
}

static value j_parse_value(jparser *J);

static value j_parse_string(jparser *J) {
    /* assumes J->p points at opening quote */
    J->p++;  /* skip " */
    sbuf sb; sbuf_init(&sb);
    while (J->p < J->end && *J->p != '"') {
        char c = *J->p++;
        if (c == '\\' && J->p < J->end) {
            char e = *J->p++;
            switch (e) {
                case 'n':  sbuf_putc(&sb, '\n'); break;
                case 't':  sbuf_putc(&sb, '\t'); break;
                case 'r':  sbuf_putc(&sb, '\r'); break;
                case 'b':  sbuf_putc(&sb, '\b'); break;
                case 'f':  sbuf_putc(&sb, '\f'); break;
                case '\\': sbuf_putc(&sb, '\\'); break;
                case '/':  sbuf_putc(&sb, '/'); break;
                case '"':  sbuf_putc(&sb, '"'); break;
                case 'u': {
                    /* \uXXXX — basic support, ASCII range only */
                    if (J->p + 4 <= J->end) {
                        char hex[5] = { J->p[0], J->p[1], J->p[2], J->p[3], 0 };
                        unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                        J->p += 4;
                        if (cp < 0x80) sbuf_putc(&sb, (char)cp);
                        else if (cp < 0x800) {
                            sbuf_putc(&sb, (char)(0xC0 | (cp >> 6)));
                            sbuf_putc(&sb, (char)(0x80 | (cp & 0x3F)));
                        } else {
                            sbuf_putc(&sb, (char)(0xE0 | (cp >> 12)));
                            sbuf_putc(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                            sbuf_putc(&sb, (char)(0x80 | (cp & 0x3F)));
                        }
                    }
                    break;
                }
                default: sbuf_putc(&sb, e); break;
            }
        } else {
            sbuf_putc(&sb, c);
        }
    }
    if (J->p >= J->end || *J->p != '"') {
        j_error(J, "unterminated string");
        free(sb.data);
        return v_nil();
    }
    J->p++;  /* skip closing " */
    return v_str_take(sb.data ? sb.data : strdup(""));
}

static value j_parse_number(jparser *J) {
    const char *start = J->p;
    int is_float = 0;
    if (J->p < J->end && *J->p == '-') J->p++;
    while (J->p < J->end && isdigit((unsigned char)*J->p)) J->p++;
    if (J->p < J->end && *J->p == '.') {
        is_float = 1;
        J->p++;
        while (J->p < J->end && isdigit((unsigned char)*J->p)) J->p++;
    }
    if (J->p < J->end && (*J->p == 'e' || *J->p == 'E')) {
        is_float = 1;
        J->p++;
        if (J->p < J->end && (*J->p == '+' || *J->p == '-')) J->p++;
        while (J->p < J->end && isdigit((unsigned char)*J->p)) J->p++;
    }
    size_t n = (size_t)(J->p - start);
    char buf[64];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    if (is_float) return v_float(atof(buf));
    return v_int((int64_t)strtoll(buf, NULL, 10));
}

static value j_parse_array(jparser *J) {
    /* assumes J->p points at [ */
    J->p++;
    value arr = v_arr();
    varr *a = arr.as.arr;
    j_skip_ws(J);
    if (J->p < J->end && *J->p == ']') { J->p++; return arr; }
    while (1) {
        j_skip_ws(J);
        value v = j_parse_value(J);
        if (J->err) { return arr; }
        if (a->len >= a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->items = realloc(a->items, a->cap * sizeof(value));
        }
        a->items[a->len++] = v;
        j_skip_ws(J);
        if (J->p >= J->end) { j_error(J, "unterminated array"); return arr; }
        if (*J->p == ']') { J->p++; break; }
        if (*J->p != ',') { j_error(J, "expected ',' or ']' in array"); return arr; }
        J->p++;
    }
    return arr;
}

static value j_parse_object(jparser *J) {
    /* assumes J->p points at { */
    J->p++;
    value d = v_dict_value();
    vdict *dict = d.as.dict;
    j_skip_ws(J);
    if (J->p < J->end && *J->p == '}') { J->p++; return d; }
    while (1) {
        j_skip_ws(J);
        if (J->p >= J->end || *J->p != '"') {
            j_error(J, "expected string key in object");
            return d;
        }
        value key = j_parse_string(J);
        if (J->err) { return d; }
        j_skip_ws(J);
        if (J->p >= J->end || *J->p != ':') {
            j_error(J, "expected ':' after key");
            return d;
        }
        J->p++;
        j_skip_ws(J);
        value val = j_parse_value(J);
        if (J->err) { return d; }
        dict_set(dict, key.as.s, val);
        j_skip_ws(J);
        if (J->p >= J->end) { j_error(J, "unterminated object"); return d; }
        if (*J->p == '}') { J->p++; break; }
        if (*J->p != ',') { j_error(J, "expected ',' or '}' in object"); return d; }
        J->p++;
    }
    return d;
}

static value j_parse_value(jparser *J) {
    j_skip_ws(J);
    if (J->p >= J->end) { j_error(J, "unexpected end of input"); return v_nil(); }
    char c = *J->p;
    if (c == '"') return j_parse_string(J);
    if (c == '{') return j_parse_object(J);
    if (c == '[') return j_parse_array(J);
    if (c == '-' || (c >= '0' && c <= '9')) return j_parse_number(J);
    if (c == 't' && J->p + 4 <= J->end && memcmp(J->p, "true", 4) == 0) {
        J->p += 4;
        return v_int(1);
    }
    if (c == 'f' && J->p + 5 <= J->end && memcmp(J->p, "false", 5) == 0) {
        J->p += 5;
        return v_int(0);
    }
    if (c == 'n' && J->p + 4 <= J->end && memcmp(J->p, "null", 4) == 0) {
        J->p += 4;
        return v_nil();
    }
    j_error(J, "unexpected character '%c'", c);
    return v_nil();
}

int json_parse_value(const char *src, value *out) {
    jparser J = {0};
    J.p = src;
    J.end = src + strlen(src);
    value v = j_parse_value(&J);
    if (J.err) {
        g_set_error("json: %s at offset %ld", J.errmsg, (long)(J.p - src));
        return -1;
    }
    *out = v;
    return 0;
}

/* ================================================================== */
/* Serializer                                                         */
/* ================================================================== */

static void j_stringify_string(sbuf *sb, const char *s) {
    sbuf_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  sbuf_puts(sb, "\\\""); break;
            case '\\': sbuf_puts(sb, "\\\\"); break;
            case '\n': sbuf_puts(sb, "\\n"); break;
            case '\r': sbuf_puts(sb, "\\r"); break;
            case '\t': sbuf_puts(sb, "\\t"); break;
            case '\b': sbuf_puts(sb, "\\b"); break;
            case '\f': sbuf_puts(sb, "\\f"); break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof hex, "\\u%04x", c);
                    sbuf_puts(sb, hex);
                } else {
                    sbuf_putc(sb, (char)c);
                }
                break;
        }
    }
    sbuf_putc(sb, '"');
}

static void j_stringify_value(sbuf *sb, const value *v, int pretty, int depth) {
    if (!v) { sbuf_puts(sb, "null"); return; }
    switch (v->kind) {
        case V_INT:
            sbuf_printf(sb, "%ld", (long)v->as.i);
            break;
        case V_FLOAT: {
            char buf[64];
            snprintf(buf, sizeof buf, "%.17g", v->as.f);
            /* ensure it looks like a float (has . or e) */
            if (!strpbrk(buf, ".eE")) {
                strncat(buf, ".0", sizeof(buf) - strlen(buf) - 1);
            }
            sbuf_puts(sb, buf);
            break;
        }
        case V_STRING:
            j_stringify_string(sb, v->as.s ? v->as.s : "");
            break;
        case V_NIL:
            sbuf_puts(sb, "null");
            break;
        case V_ARRAY: {
            varr *a = v->as.arr;
            sbuf_putc(sb, '[');
            for (size_t i = 0; i < a->len; i++) {
                if (i) sbuf_putc(sb, ',');
                if (pretty) { sbuf_putc(sb, '\n'); for (int d=0; d<depth+1; d++) sbuf_puts(sb, "  "); }
                j_stringify_value(sb, &a->items[i], pretty, depth+1);
            }
            if (pretty && a->len) { sbuf_putc(sb, '\n'); for (int d=0; d<depth; d++) sbuf_puts(sb, "  "); }
            sbuf_putc(sb, ']');
            break;
        }
        case V_DICT: {
            vdict *d = v->as.dict;
            sbuf_putc(sb, '{');
            int first = 1;
            for (size_t i = 0; i < d->cap; i++) {
                if (d->keys[i]) {
                    if (!first) sbuf_putc(sb, ',');
                    first = 0;
                    if (pretty) { sbuf_putc(sb, '\n'); for (int dd=0; dd<depth+1; dd++) sbuf_puts(sb, "  "); }
                    j_stringify_string(sb, d->keys[i]);
                    sbuf_putc(sb, ':');
                    if (pretty) sbuf_putc(sb, ' ');
                    j_stringify_value(sb, &d->vals[i], pretty, depth+1);
                }
            }
            if (pretty && !first) { sbuf_putc(sb, '\n'); for (int dd=0; dd<depth; dd++) sbuf_puts(sb, "  "); }
            sbuf_putc(sb, '}');
            break;
        }
        case V_PTR:
            sbuf_printf(sb, "\"<ptr:0x%lx>\"", (unsigned long)v->as.ptr);
            break;
        case V_FUNC:
        case V_NATIVE:
            sbuf_puts(sb, "\"<function>\"");
            break;
    }
}

char *json_stringify_value(const value *v, int pretty, int indent) {
    (void)indent;
    sbuf sb; sbuf_init(&sb);
    j_stringify_value(&sb, v, pretty, 0);
    return sb.data ? sb.data : strdup("");
}

/* ================================================================== */
/* Builtins                                                           */
/* ================================================================== */

static value n_json_parse(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_STRING) {
        g_set_error("json_parse(s) needs a string");
        return v_nil();
    }
    value out;
    if (json_parse_value(argv[0].as.s, &out) != 0) {
        return v_nil();
    }
    return out;
}

static value n_json_stringify(int argc, value *argv) {
    if (argc != 1) {
        g_set_error("json_stringify(v) needs one arg");
        return v_str("");
    }
    char *s = json_stringify_value(&argv[0], 0, 0);
    return v_str_take(s);
}

static value n_json_stringify_pretty(int argc, value *argv) {
    if (argc != 1) {
        g_set_error("json_stringify_pretty(v) needs one arg");
        return v_str("");
    }
    char *s = json_stringify_value(&argv[0], 1, 0);
    return v_str_take(s);
}

native_fn json_nb_parse(void)            { return n_json_parse; }
native_fn json_nb_stringify(void)       { return n_json_stringify; }
native_fn json_nb_stringify_pretty(void){ return n_json_stringify_pretty; }
