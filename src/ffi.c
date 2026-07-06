/* ffi.c — language-agnostic FFI for Glyph.
 *
 * Plan 9 / Unix philosophy: every language is a filter. We don't write
 * a special binding layer for each language — we exec the language's
 * interpreter/compiler as a subprocess and speak a tiny line-delimited
 * JSON protocol over its stdin/stdout.
 *
 * Three layers, each built on the one below:
 *
 *   1. exec(cmd, input)             — run a command, capture output
 *   2. pipe_open / pipe_readln /    — persistent subprocess for
 *      pipe_write / pipe_close        high-frequency calls
 *   3. lang_eval(lang, code)        — high-level helpers that use
 *      lang_call(lang, mod, fn, args)  per-language adapter scripts
 *                                      in $GLYPH_LIB/lang/
 *
 * The adapter scripts (one per language: python, node, rust, zig, nim,
 * elm, cpp) read JSON requests on stdin and write JSON responses on
 * stdout. They live in lib/lang/<lang>.sh and are intentionally tiny.
 *
 * Author: Glyph FFI extension
 */
#include "glyph.h"
#include "platform.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifdef GLYPH_UNIX
#  include <poll.h>
#endif

/* ------------------------------------------------------------------ */
/* pipe handle — opaque to Glyph (V_PTR)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    pid_t  pid;
    int    to_fd;     /* write end - we write, child reads from stdin */
    int    from_fd;   /* read end - child writes to stdout, we read   */
    FILE  *to_fp;
    FILE  *from_fp;
    char  *lang;      /* for lang_call/lang_eval: which adapter */
} g_pipe;

/* ------------------------------------------------------------------ */
/* exec(cmd, input) -> string                                          */
/* Run cmd with input on stdin, return stdout.                         */
/* ------------------------------------------------------------------ */

static value nb_exec(int argc, value *argv) {
    if (argc < 1 || argc > 2 || argv[0].kind != V_STRING) {
        g_set_error("exec(cmd, input?) needs a string command");
        return v_nil();
    }
    const char *cmd = argv[0].as.s;
    const char *input = (argc == 2 && argv[1].kind == V_STRING) ? argv[1].as.s : NULL;

#ifdef GLYPH_UNIX
    /* Use fork + pipes so we can properly close stdin and read until EOF */
    int in_pipe[2];   /* parent writes -> child stdin */
    int out_pipe[2];  /* child stdout -> parent reads */
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        g_set_error("exec: pipe failed: %s", strerror(errno));
        return v_nil();
    }
    pid_t pid = fork();
    if (pid < 0) {
        g_set_error("exec: fork failed: %s", strerror(errno));
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return v_nil();
    }
    if (pid == 0) {
        /* child */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);  /* capture stderr too */
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (input) {
        write(in_pipe[1], input, strlen(input));
    }
    close(in_pipe[1]);  /* signal EOF to child */

    sbuf out; sbuf_init(&out);
    char buf[4096];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof buf)) > 0) {
        sbuf_putn(&out, buf, (size_t)n);
    }
    close(out_pipe[0]);
    int status;
    waitpid(pid, &status, 0);
    return v_str_take(out.data ? out.data : strdup(""));
#else
    /* Windows fallback using popen */
    FILE *p = popen(cmd, "r");
    if (!p) {
        g_set_error("exec: popen failed: %s", strerror(errno));
        return v_nil();
    }
    sbuf out; sbuf_init(&out);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
        sbuf_putn(&out, buf, n);
    }
    pclose(p);
    return v_str_take(out.data ? out.data : strdup(""));
#endif
}

/* ------------------------------------------------------------------ */
/* exec_status(cmd, input) -> int                                      */
/* Same as exec but returns exit code instead of output.               */
/* ------------------------------------------------------------------ */

static value nb_exec_status(int argc, value *argv) {
    if (argc < 1 || argc > 2 || argv[0].kind != V_STRING) {
        g_set_error("exec_status(cmd, input?) needs a string command");
        return v_int(-1);
    }
    const char *cmd = argv[0].as.s;
    const char *input = (argc == 2 && argv[1].kind == V_STRING) ? argv[1].as.s : NULL;

#ifdef GLYPH_UNIX
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return v_int(-1);
    pid_t pid = fork();
    if (pid < 0) return v_int(-1);
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[0]); close(out_pipe[1]);
    if (input) write(in_pipe[1], input, strlen(input));
    close(in_pipe[1]);
    int status;
    waitpid(pid, &status, 0);
    return v_int(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
#else
    char *full_cmd;
    if (asprintf(&full_cmd, "%s >/dev/null 2>&1", cmd) < 0) return v_int(-1);
    FILE *p = popen(full_cmd, input ? "w" : "r");
    free(full_cmd);
    if (!p) return v_int(-1);
    if (input) { fputs(input, p); fflush(p); }
    int status = pclose(p);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return v_int(exit_code);
#endif
}

/* ------------------------------------------------------------------ */
/* Helper: run a command, feed it input, capture stdout.               */
/* Used by lang_eval and lang_call. Returns malloc'd string.           */
/* ------------------------------------------------------------------ */

static char *exec_capture(const char *cmd, const char *input, size_t input_len) {
#ifdef GLYPH_UNIX
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        /* discard stderr to avoid polluting our protocol */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    if (input && input_len > 0) {
        size_t written = 0;
        while (written < input_len) {
            ssize_t n = write(in_pipe[1], input + written, input_len - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
    }
    close(in_pipe[1]);

    sbuf out; sbuf_init(&out);
    char buf[4096];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof buf)) > 0) {
        sbuf_putn(&out, buf, (size_t)n);
    }
    close(out_pipe[0]);
    int status;
    waitpid(pid, &status, 0);
    return out.data ? out.data : strdup("");
#else
    /* Windows fallback */
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    sbuf out; sbuf_init(&out);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) sbuf_putn(&out, buf, n);
    pclose(p);
    return out.data ? out.data : strdup("");
#endif
}

/* ------------------------------------------------------------------ */
/* pipe_open(cmd) -> ptr                                               */
/* Spawn a subprocess with bidirectional pipe. Returns opaque handle.  */
/* ------------------------------------------------------------------ */

static value nb_pipe_open(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_STRING) {
        g_set_error("pipe_open(cmd) needs a string command");
        return v_ptr(NULL);
    }
    const char *cmd = argv[0].as.s;

#ifdef GLYPH_UNIX
    int to_child[2];    /* parent writes -> child stdin  */
    int from_child[2];  /* child stdout -> parent reads   */
    if (pipe(to_child) < 0 || pipe(from_child) < 0) {
        g_set_error("pipe_open: pipe() failed: %s", strerror(errno));
        return v_ptr(NULL);
    }
    pid_t pid = fork();
    if (pid < 0) {
        g_set_error("pipe_open: fork() failed: %s", strerror(errno));
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return v_ptr(NULL);
    }
    if (pid == 0) {
        /* child: wire up stdin/stdout, then exec shell -c cmd */
        dup2(to_child[0], 0);
        dup2(from_child[1], 1);
        /* stderr goes to parent's stderr so error messages propagate */
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    /* parent */
    close(to_child[0]);
    close(from_child[1]);
    g_pipe *h = calloc(1, sizeof(g_pipe));
    h->pid = pid;
    h->to_fd = to_child[1];
    h->from_fd = from_child[0];
    h->to_fp = fdopen(h->to_fd, "w");
    h->from_fp = fdopen(h->from_fd, "r");
    if (!h->to_fp || !h->from_fp) {
        g_set_error("pipe_open: fdopen failed");
        free(h);
        return v_ptr(NULL);
    }
    setvbuf(h->to_fp, NULL, _IOLBF, 0);
    setvbuf(h->from_fp, NULL, _IOLBF, 0);
    return v_ptr(h);
#else
    g_set_error("pipe_open not supported on Windows yet");
    return v_ptr(NULL);
#endif
}

/* ------------------------------------------------------------------ */
/* pipe_write(h, s) -> int (bytes written, or -1 on error)             */
/* ------------------------------------------------------------------ */

static value nb_pipe_write(int argc, value *argv) {
    if (argc != 2 || argv[0].kind != V_PTR || argv[1].kind != V_STRING) {
        g_set_error("pipe_write(handle, str) needs a ptr and a string");
        return v_int(-1);
    }
    g_pipe *h = (g_pipe*)argv[0].as.ptr;
    if (!h || !h->to_fp) return v_int(-1);
    const char *s = argv[1].as.s;
    size_t len = strlen(s);
    size_t n = fwrite(s, 1, len, h->to_fp);
    fflush(h->to_fp);
    return v_int((int64_t)n);
}

/* ------------------------------------------------------------------ */
/* pipe_readln(h) -> string (one line, without newline)                */
/* Returns nil on EOF.                                                 */
/* ------------------------------------------------------------------ */

static value nb_pipe_readln(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_PTR) {
        g_set_error("pipe_readln(handle) needs a ptr");
        return v_nil();
    }
    g_pipe *h = (g_pipe*)argv[0].as.ptr;
    if (!h || !h->from_fp) return v_nil();

    sbuf sb; sbuf_init(&sb);
    int c;
    while ((c = fgetc(h->from_fp)) != EOF) {
        if (c == '\n') break;
        sbuf_putc(&sb, (char)c);
    }
    if (c == EOF && sb.len == 0) {
        free(sb.data);
        return v_nil();
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

/* ------------------------------------------------------------------ */
/* pipe_read(h, n) -> string (up to n bytes)                           */
/* ------------------------------------------------------------------ */

static value nb_pipe_read(int argc, value *argv) {
    if (argc != 2 || argv[0].kind != V_PTR || argv[1].kind != V_INT) {
        g_set_error("pipe_read(handle, n) needs a ptr and an int");
        return v_nil();
    }
    g_pipe *h = (g_pipe*)argv[0].as.ptr;
    if (!h || !h->from_fp) return v_nil();
    int64_t want = argv[1].as.i;
    if (want < 0 || want > (1<<20)) want = 4096;

    sbuf sb; sbuf_init(&sb);
    char buf[4096];
    int64_t got = 0;
    while (got < want) {
        int64_t chunk = want - got;
        if (chunk > (int64_t)sizeof buf) chunk = sizeof buf;
        /* Use fgets-style read with timeout via select */
#ifdef GLYPH_UNIX
        struct pollfd pfd = { .fd = h->from_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);  /* 100ms timeout */
        if (pr <= 0) break;  /* timeout or error */
        if (!(pfd.revents & POLLIN)) break;
#endif
        size_t n = fread(buf, 1, (size_t)chunk, h->from_fp);
        if (n == 0) break;
        sbuf_putn(&sb, buf, n);
        got += n;
        if (n < (size_t)chunk) break;  /* short read = no more data */
    }
    return v_str_take(sb.data ? sb.data : strdup(""));
}

/* ------------------------------------------------------------------ */
/* pipe_close(h) -> int (exit status)                                  */
/* ------------------------------------------------------------------ */

static value nb_pipe_close(int argc, value *argv) {
    if (argc != 1 || argv[0].kind != V_PTR) {
        g_set_error("pipe_close(handle) needs a ptr");
        return v_int(-1);
    }
    g_pipe *h = (g_pipe*)argv[0].as.ptr;
    if (!h) return v_int(-1);
    if (h->to_fp) { fclose(h->to_fp); h->to_fp = NULL; }
    if (h->from_fp) { fclose(h->from_fp); h->from_fp = NULL; }
    int status = 0;
#ifdef GLYPH_UNIX
    if (h->pid > 0) {
        int st;
        waitpid(h->pid, &st, 0);
        status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
#endif
    free(h->lang);
    free(h);
    return v_int(status);
}

/* ------------------------------------------------------------------ */
/* Helper: locate the language adapter script                          */
/* Returns malloc'd path or NULL.                                      */
/* ------------------------------------------------------------------ */

static char *find_lang_adapter(const char *lang) {
    /* Search order:
     *   1. $GLYPH_LIB/lang/<lang>.sh
     *   2. <exe_dir>/../lib/lang/<lang>.sh
     *   3. /usr/local/lib/glyph/lang/<lang>.sh
     *   4. /usr/lib/glyph/lang/<lang>.sh
     */
    const char *env = getenv("GLYPH_LIB");
    if (env) {
        char *p;
        if (asprintf(&p, "%s/lang/%s.sh", env, lang) >= 0) {
            if (glyph_file_exists(p)) return p;
            free(p);
        }
    }
    /* Try relative to exe */
    char exe_path[4096];
    ssize_t n = GLYPH_READ_SELF_PATH(exe_path, sizeof exe_path - 1);
    if (n > 0) {
        exe_path[n] = 0;
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = 0;
            char *p;
            if (asprintf(&p, "%s/../lib/lang/%s.sh", exe_path, lang) >= 0) {
                if (glyph_file_exists(p)) return p;
                free(p);
            }
        }
    }
    /* System locations */
    const char *sys_paths[] = {
        "/usr/local/lib/glyph/lang/%s.sh",
        "/usr/lib/glyph/lang/%s.sh",
        NULL
    };
    for (int i = 0; sys_paths[i]; i++) {
        char *p;
        if (asprintf(&p, sys_paths[i], lang) >= 0) {
            if (glyph_file_exists(p)) return p;
            free(p);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lang_eval(lang, code) -> string                                     */
/* Evaluate `code` in `lang`. Returns the result as a string.          */
/* Uses the per-language adapter script.                               */
/* ------------------------------------------------------------------ */

static value nb_lang_eval(int argc, value *argv) {
    if (argc != 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING) {
        g_set_error("lang_eval(lang, code) needs two strings");
        return v_nil();
    }
    const char *lang = argv[0].as.s;
    const char *code = argv[1].as.s;

    char *adapter = find_lang_adapter(lang);
    if (!adapter) {
        g_set_error("lang_eval: no adapter for language '%s' "
                    "(set GLYPH_LIB or install to /usr/local/lib/glyph/lang/)",
                    lang);
        return v_nil();
    }

    /* Build JSON request: {"op":"eval","code":"..."} */
    sbuf req; sbuf_init(&req);
    sbuf_puts(&req, "{\"op\":\"eval\",\"code\":\"");
    for (const char *p = code; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"') { sbuf_putc(&req, '\\'); sbuf_putc(&req, c); }
        else if (c == '\n') sbuf_puts(&req, "\\n");
        else if (c == '\r') sbuf_puts(&req, "\\r");
        else if (c == '\t') sbuf_puts(&req, "\\t");
        else if (c < 0x20) sbuf_printf(&req, "\\u%04x", c);
        else sbuf_putc(&req, c);
    }
    sbuf_puts(&req, "\"}\n");

    char *resp_data = exec_capture(adapter, req.data, req.len);
    free(adapter);
    free(req.data);

    if (!resp_data) return v_str("");

    /* Parse JSON response: find "value":"..." or "error":"..." */
    /* Tolerant of whitespace: "value":"..." or "value": "..." */
    char *val_p = strstr(resp_data, "\"value\"");
    char *err_p = strstr(resp_data, "\"error\"");
    if (err_p && (!val_p || err_p < val_p)) {
        err_p = strchr(err_p + 7, ':');
        if (!err_p) { free(resp_data); return v_nil(); }
        err_p++;
        while (*err_p == ' ' || *err_p == '\t') err_p++;
        if (*err_p != '"') { free(resp_data); return v_nil(); }
        err_p++;
        sbuf msg; sbuf_init(&msg);
        while (*err_p && *err_p != '"') {
            if (*err_p == '\\') {
                err_p++;
                if (*err_p == 'n') sbuf_putc(&msg, '\n');
                else if (*err_p == 't') sbuf_putc(&msg, '\t');
                else sbuf_putc(&msg, *err_p);
            } else sbuf_putc(&msg, *err_p);
            err_p++;
        }
        g_set_error("lang_eval error: %s", msg.data ? msg.data : "(unknown)");
        free(msg.data); free(resp_data);
        return v_nil();
    }
    if (val_p) {
        val_p = strchr(val_p + 7, ':');
        if (!val_p) { free(resp_data); return v_str(""); }
        val_p++;
        while (*val_p == ' ' || *val_p == '\t') val_p++;
        if (*val_p != '"') { free(resp_data); return v_str(""); }
        val_p++;
        sbuf val; sbuf_init(&val);
        while (*val_p && *val_p != '"') {
            if (*val_p == '\\') {
                val_p++;
                if (*val_p == 'n') sbuf_putc(&val, '\n');
                else if (*val_p == 't') sbuf_putc(&val, '\t');
                else sbuf_putc(&val, *val_p);
            } else sbuf_putc(&val, *val_p);
            val_p++;
        }
        free(resp_data);
        return v_str_take(val.data ? val.data : strdup(""));
    }
    return v_str_take(resp_data);
}

/* ------------------------------------------------------------------ */
/* lang_call(lang, module, function, args_array) -> string             */
/* Call function `function` in module `module` of language `lang`.     */
/* args_array is an array of values. Returns result as string.         */
/* ------------------------------------------------------------------ */

static value nb_lang_call(int argc, value *argv) {
    if (argc != 4 ||
        argv[0].kind != V_STRING ||
        argv[1].kind != V_STRING ||
        argv[2].kind != V_STRING ||
        argv[3].kind != V_ARRAY) {
        g_set_error("lang_call(lang, module, function, args_array) needs "
                    "(string, string, string, array)");
        return v_nil();
    }
    const char *lang    = argv[0].as.s;
    const char *module  = argv[1].as.s;
    const char *function= argv[2].as.s;
    varr       *args    = argv[3].as.arr;

    char *adapter = find_lang_adapter(lang);
    if (!adapter) {
        g_set_error("lang_call: no adapter for language '%s'", lang);
        return v_nil();
    }

    /* Build JSON: {"op":"call","module":"...","function":"...","args":[...]} */
    sbuf req; sbuf_init(&req);
    sbuf_puts(&req, "{\"op\":\"call\",\"module\":\"");
    sbuf_puts(&req, module);
    sbuf_puts(&req, "\",\"function\":\"");
    sbuf_puts(&req, function);
    sbuf_puts(&req, "\",\"args\":[");
    for (size_t i = 0; i < args->len; i++) {
        if (i) sbuf_putc(&req, ',');
        value *v = &args->items[i];
        switch (v->kind) {
            case V_INT:
                sbuf_printf(&req, "%ld", (long)v->as.i);
                break;
            case V_FLOAT: {
                /* print as JSON number */
                char numbuf[64];
                snprintf(numbuf, sizeof numbuf, "%.17g", v->as.f);
                sbuf_puts(&req, numbuf);
                break;
            }
            case V_STRING:
                sbuf_putc(&req, '"');
                for (const char *p = v->as.s; *p; p++) {
                    unsigned char c = (unsigned char)*p;
                    if (c == '\\' || c == '"') { sbuf_putc(&req, '\\'); sbuf_putc(&req, c); }
                    else if (c == '\n') sbuf_puts(&req, "\\n");
                    else if (c == '\r') sbuf_puts(&req, "\\r");
                    else if (c == '\t') sbuf_puts(&req, "\\t");
                    else if (c < 0x20) sbuf_printf(&req, "\\u%04x", c);
                    else sbuf_putc(&req, c);
                }
                sbuf_putc(&req, '"');
                break;
            case V_NIL:
                sbuf_puts(&req, "null");
                break;
            case V_PTR:
                sbuf_printf(&req, "\"<ptr:0x%lx>\"", (unsigned long)v->as.ptr);
                break;
            default:
                sbuf_puts(&req, "null");
                break;
        }
    }
    sbuf_puts(&req, "]}\n");

    /* exec adapter */
    char *resp_data = exec_capture(adapter, req.data, req.len);
    free(adapter);
    free(req.data);
    if (!resp_data) return v_str("");

    /* extract value or error (same as lang_eval) */
    char *val_p = strstr(resp_data, "\"value\"");
    char *err_p = strstr(resp_data, "\"error\"");
    if (err_p && (!val_p || err_p < val_p)) {
        err_p = strchr(err_p + 7, ':');
        if (!err_p) { free(resp_data); return v_nil(); }
        err_p++;
        while (*err_p == ' ' || *err_p == '\t') err_p++;
        if (*err_p != '"') { free(resp_data); return v_nil(); }
        err_p++;
        sbuf msg; sbuf_init(&msg);
        while (*err_p && *err_p != '"') {
            if (*err_p == '\\') {
                err_p++;
                if (*err_p == 'n') sbuf_putc(&msg, '\n');
                else if (*err_p == 't') sbuf_putc(&msg, '\t');
                else sbuf_putc(&msg, *err_p);
            } else sbuf_putc(&msg, *err_p);
            err_p++;
        }
        g_set_error("lang_call error: %s", msg.data ? msg.data : "(unknown)");
        free(msg.data); free(resp_data);
        return v_nil();
    }
    if (val_p) {
        val_p = strchr(val_p + 7, ':');
        if (!val_p) { free(resp_data); return v_str(""); }
        val_p++;
        while (*val_p == ' ' || *val_p == '\t') val_p++;
        if (*val_p != '"') { free(resp_data); return v_str(""); }
        val_p++;
        sbuf val; sbuf_init(&val);
        while (*val_p && *val_p != '"') {
            if (*val_p == '\\') {
                val_p++;
                if (*val_p == 'n') sbuf_putc(&val, '\n');
                else if (*val_p == 't') sbuf_putc(&val, '\t');
                else sbuf_putc(&val, *val_p);
            } else sbuf_putc(&val, *val_p);
            val_p++;
        }
        free(resp_data);
        return v_str_take(val.data ? val.data : strdup(""));
    }
    return v_str_take(resp_data);
}

/* ------------------------------------------------------------------ */
/* lang_list() -> array of strings                                     */
/* Returns the names of available language adapters.                   */
/* ------------------------------------------------------------------ */

static value nb_lang_list(int argc, value *argv) {
    (void)argc; (void)argv;
    value arr = v_arr();

    /* Check each known language's adapter */
    const char *langs[] = {
        "python", "python3", "node", "rust", "zig", "nim",
        "elm", "cpp", "c", "go", "ruby", "perl", "lua",
        "julia", "tcl", "awk", "sed", "bash", NULL
    };
    for (int i = 0; langs[i]; i++) {
        char *p = find_lang_adapter(langs[i]);
        if (p) {
            /* grow array manually — push() is in interp.c, not exported */
            varr *a = arr.as.arr;
            if (a->len >= a->cap) {
                a->cap = a->cap ? a->cap * 2 : 8;
                a->items = realloc(a->items, a->cap * sizeof(value));
            }
            a->items[a->len++] = v_str(langs[i]);
            free(p);
        }
    }
    return arr;
}

/* ------------------------------------------------------------------ */
/* Public: registration is done by interp.c via ffi_install_builtins, */
/* which has access to the private `interp` struct. This file just    */
/* exposes the native function pointers.                              */
/* ------------------------------------------------------------------ */

native_fn ffi_nb_exec(void)        { return nb_exec; }
native_fn ffi_nb_exec_status(void) { return nb_exec_status; }
native_fn ffi_nb_pipe_open(void)   { return nb_pipe_open; }
native_fn ffi_nb_pipe_write(void)  { return nb_pipe_write; }
native_fn ffi_nb_pipe_readln(void) { return nb_pipe_readln; }
native_fn ffi_nb_pipe_read(void)   { return nb_pipe_read; }
native_fn ffi_nb_pipe_close(void)  { return nb_pipe_close; }
native_fn ffi_nb_lang_eval(void)   { return nb_lang_eval; }
native_fn ffi_nb_lang_call(void)   { return nb_lang_call; }
native_fn ffi_nb_lang_list(void)   { return nb_lang_list; }
