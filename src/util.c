/* util.c — sbuf, pvec, error reporting.
 *
 * No external dependencies. Pure C11.
 */

#include "glyph.h"
#include "platform.h"

/* ------------------------------------------------------------------ */
/* error reporting (thread-local last error)                          */
/* ------------------------------------------------------------------ */

static __thread char g_errbuf[1024];

const char *g_last_error(void) {
    return g_errbuf;
}

void g_clear_error(void) {
    g_errbuf[0] = '\0';
}

void g_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_errbuf, sizeof(g_errbuf), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* sbuf                                                                */
/* ------------------------------------------------------------------ */

void sbuf_init(sbuf *s) {
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void sbuf_free(sbuf *s) {
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

static void sbuf_reserve(sbuf *s, size_t add) {
    if (s->len + add + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 64;
        while (s->len + add + 1 > ncap) ncap *= 2;
        s->data = realloc(s->data, ncap);
        s->cap = ncap;
    }
}

void sbuf_putc(sbuf *s, char c) {
    sbuf_reserve(s, 1);
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

void sbuf_putn(sbuf *s, const char *data, size_t n) {
    sbuf_reserve(s, n);
    memcpy(s->data + s->len, data, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void sbuf_puts(sbuf *s, const char *str) {
    sbuf_putn(s, str, strlen(str));
}

void sbuf_printf(sbuf *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sbuf_reserve(s, (size_t)n);
    vsnprintf(s->data + s->len, s->cap - s->len, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

/* ------------------------------------------------------------------ */
/* pvec                                                                */
/* ------------------------------------------------------------------ */

void pvec_init(pvec *v) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

void pvec_free(pvec *v) {
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}

void pvec_push(pvec *v, void *p) {
    if (v->len + 1 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = realloc(v->data, v->cap * sizeof(void*));
    }
    v->data[v->len++] = p;
}

void *pvec_pop(pvec *v) {
    if (v->len == 0) return NULL;
    return v->data[--v->len];
}
