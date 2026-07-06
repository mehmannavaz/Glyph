/* lex.c — Glyph lexer.
 *
 * Converts source text into a flat token list.
 *
 * Responsibilities:
 *   - recognize the four shape delimiters as distinct tokens
 *   - recognize keywords vs identifiers
 *   - integer / float / string literals (with escapes)
 *   - operators, including 2-char ones (==, !=, <=, >=, &&, ||, <<, >>, ..)
 *   - comments (# until end of line)
 *   - track line/col for diagnostics
 *
 * Indentation (INDENT/DEDENT) is NOT lexed here. Glyph uses explicit shape
 * brackets to delineate blocks; indentation is stylistic only. This avoids
 * the famous Python tab/space problem while keeping visual readability.
 */

#include "glyph.h"
#include "platform.h"

static const struct { const char *kw; token_kind k; } KEYWORDS[] = {
    {"return",  T_KW_RETURN},
    {"stop",    T_KW_STOP},
    {"next",    T_KW_NEXT},
    {"raise",   T_KW_RAISE},
    {"print",   T_KW_PRINT},
    {"the",     T_KW_THE},
    {"if",      T_KW_IF},
    {"else",    T_KW_ELSE},
    {"elif",    T_KW_ELIF},
    {"when",    T_KW_WHEN},
    {"for",     T_KW_FOR},
    {"in",      T_KW_IN},
    {"true",    T_KW_TRUE},
    {"false",   T_KW_FALSE},
    {"nil",     T_KW_NIL},
};
static const int NKEYWORDS = sizeof(KEYWORDS)/sizeof(KEYWORDS[0]);

typedef struct {
    const char *src;
    const char *srcname;
    const char *p;
    int line;
    int col;
    tokenlist  *out;
    int at_stmt_start;  /* 1 if next non-ws token is at statement start */
} lexer;

static void emit(lexer *L, token_kind k) {
    token *t = calloc(1, sizeof(token));
    t->kind = k;
    t->line = L->line;
    t->col  = L->col;
    if (L->out->len + 1 > L->out->cap) {
        L->out->cap = L->out->cap ? L->out->cap*2 : 64;
        L->out->items = realloc(L->out->items, L->out->cap * sizeof(token));
    }
    L->out->items[L->out->len++] = *t;
    free(t);
    /* After NEWLINE or SEMI, next token is at statement start */
    if (k == T_NEWLINE || k == T_SEMI) L->at_stmt_start = 1;
    else L->at_stmt_start = 0;
}

static void emit_str(lexer *L, token_kind k, char *s) {
    emit(L, k);
    L->out->items[L->out->len-1].text = s;
}

static void emit_int(lexer *L, int64_t v) {
    emit(L, T_INT);
    L->out->items[L->out->len-1].ival = v;
}

static void emit_float(lexer *L, double v) {
    emit(L, T_FLOAT);
    L->out->items[L->out->len-1].fval = v;
}

static token_kind ident_kind(const char *s, size_t n) {
    for (int i = 0; i < NKEYWORDS; i++) {
        size_t kn = strlen(KEYWORDS[i].kw);
        if (kn == n && memcmp(KEYWORDS[i].kw, s, n) == 0)
            return KEYWORDS[i].k;
    }
    return T_IDENT;
}

static char *read_string(lexer *L) {
    /* assumes L->p points just past the opening " */
    sbuf sb; sbuf_init(&sb);
    while (*L->p && *L->p != '"') {
        char c = *L->p++;
        L->col++;
        if (c == '\n') { L->line++; L->col = 1; }
        if (c == '\\' && *L->p) {
            char e = *L->p++;
            L->col++;
            switch (e) {
                case 'n':  sbuf_putc(&sb, '\n'); break;
                case 't':  sbuf_putc(&sb, '\t'); break;
                case 'r':  sbuf_putc(&sb, '\r'); break;
                case '\\': sbuf_putc(&sb, '\\'); break;
                case '"':  sbuf_putc(&sb, '"');  break;
                case '0':  sbuf_putc(&sb, '\0'); break;
                case 'x': {
                    if (L->p[0] && L->p[1] && isxdigit((unsigned char)L->p[0]) && isxdigit((unsigned char)L->p[1])) {
                        char hex[3] = { L->p[0], L->p[1], 0 };
                        sbuf_putc(&sb, (char)strtol(hex, NULL, 16));
                        L->p += 2; L->col += 2;
                    } else {
                        sbuf_putc(&sb, e);
                    }
                    break;
                }
                default:   sbuf_putc(&sb, e); break;
            }
        } else {
            sbuf_putc(&sb, c);
        }
    }
    if (*L->p != '"') {
        g_set_error("%s:%d: unterminated string literal", L->srcname, L->line);
        sbuf_free(&sb);
        return NULL;
    }
    L->p++; L->col++;
    return sb.data ? sb.data : strdup("");
}

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident_cont(int c)  { return isalnum(c) || c == '_'; }

/* Languages recognised by the [langname] ... block syntax.
 * When the lexer sees `[<one-of-these>]` at statement start, it captures
 * the raw source text until the next block opener at the same or lower
 * column, or EOF. The captured text becomes a T_RAW_STRING token.
 *
 * Adding a new language is a one-line change here. */
static const char *FOREIGN_LANGS[] = {
    "python", "python3", "py",
    "node", "nodejs", "js", "javascript",
    "rust", "rs",
    "zig",
    "nim",
    "c", "cpp", "c++",
    "elm",
    "go", "golang",
    "ruby", "rb",
    "perl",
    "lua",
    "julia",
    "tcl",
    "bash", "sh", "shell",
    "awk", "sed",
    NULL
};

static int is_foreign_lang(const char *name) {
    for (int i = 0; FOREIGN_LANGS[i]; i++) {
        if (strcmp(FOREIGN_LANGS[i], name) == 0) return 1;
    }
    return 0;
}

/* Capture raw source text for a [lang] block body. The text starts at
 * the line AFTER the closing ] of the header, and ends at the first
 * line whose first non-whitespace character is one of `[ ( < {` at a
 * column <= opener_col, or at EOF. The captured text is returned as a
 * malloc'd string (caller frees). The lexer state (L->p, L->line,
 * L->col) is advanced past the captured text.
 *
 * The returned text has the common leading whitespace of non-empty
 * lines stripped (dedented), so the foreign code starts at column 1
 * regardless of how deeply it was nested in Glyph source. This is
 * essential for Python (which errors on unexpected indent) and nice
 * for everyone else. */
static char *capture_raw_block(lexer *L, int opener_col) {
    /* Phase 1: capture raw lines into a temporary buffer. */
    sbuf raw; sbuf_init(&raw);

    /* Skip the rest of the line containing `]` (the newline too). */
    while (*L->p && *L->p != '\n') { L->p++; L->col++; }
    if (*L->p == '\n') { L->p++; L->line++; L->col = 1; }

    /* Capture lines until dedent or EOF. */
    while (*L->p) {
        /* Count leading whitespace on this line. */
        const char *ws_end = L->p;
        int line_col = 1;
        while (*ws_end == ' ' || *ws_end == '\t') { ws_end++; line_col++; }

        /* If line is blank (just newline or EOF), copy it verbatim. */
        if (*ws_end == '\n' || *ws_end == '\0') {
            while (*L->p && *L->p != '\n') { sbuf_putc(&raw, *L->p); L->p++; L->col++; }
            if (*L->p == '\n') { sbuf_putc(&raw, '\n'); L->p++; L->line++; L->col = 1; }
            continue;
        }

        /* If this line's first non-ws char is at col <= opener_col,
         * the foreign block has ended — stop capturing. This matches
         * how Glyph squire bodies terminate (dedent = end of block).
         * It doesn't matter what the first char IS; any dedent ends
         * the block. */
        if (line_col <= opener_col) {
            break;
        }

        /* Otherwise: copy this line verbatim. */
        while (*L->p && *L->p != '\n') { sbuf_putc(&raw, *L->p); L->p++; L->col++; }
        if (*L->p == '\n') { sbuf_putc(&raw, '\n'); L->p++; L->line++; L->col = 1; }
    }

    if (!raw.data || raw.len == 0) {
        return strdup("");
    }

    /* Phase 2: compute the common leading whitespace across all
     * non-blank lines. */
    size_t common = (size_t)-1;
    const char *p = raw.data;
    while (*p) {
        /* measure leading ws of this line */
        size_t ws = 0;
        while (p[ws] == ' ' || p[ws] == '\t') ws++;
        /* if line is all-ws + newline (or end), skip it */
        if (p[ws] == '\n' || p[ws] == '\0') {
            /* skip to next line */
            p += ws;
            if (*p == '\n') p++;
            continue;
        }
        if (ws < common) common = ws;
        /* advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    if (common == (size_t)-1) common = 0;

    /* Phase 3: build the dedented output. */
    sbuf out; sbuf_init(&out);
    p = raw.data;
    while (*p) {
        /* skip up to `common` leading ws chars */
        size_t to_skip = common;
        while (to_skip > 0 && (*p == ' ' || *p == '\t')) { p++; to_skip--; }
        /* copy rest of line */
        while (*p && *p != '\n') { sbuf_putc(&out, *p); p++; }
        if (*p == '\n') { sbuf_putc(&out, '\n'); p++; }
    }

    free(raw.data);
    return out.data ? out.data : strdup("");
}

tokenlist *lex(const char *src, const char *srcname) {
    lexer L = {0};
    L.src = src;
    L.srcname = srcname ? srcname : "<stdin>";
    L.p = src;
    L.line = 1;
    L.col = 1;
    L.at_stmt_start = 1;

    tokenlist *tl = calloc(1, sizeof(tokenlist));
    tl->srcname = L.srcname;
    L.out = tl;

    while (*L.p) {
        char c = *L.p;

        /* whitespace (not newline) */
        if (c == ' ' || c == '\t' || c == '\r') {
            L.p++; L.col++; continue;
        }

        /* newline */
        if (c == '\n') {
            L.line++; L.col = 1; L.p++;
            emit(&L, T_NEWLINE);
            continue;
        }

        /* comment */
        if (c == '#') {
            while (*L.p && *L.p != '\n') { L.p++; L.col++; }
            continue;
        }

        /* string */
        if (c == '"') {
            int sline = L.line, scol = L.col;
            L.p++; L.col++;
            char *s = read_string(&L);
            if (!s) { tok_free(tl); return NULL; }
            emit_str(&L, T_STRING, s);
            tl->items[tl->len-1].line = sline;
            tl->items[tl->len-1].col  = scol;
            continue;
        }

        /* number */
        if (isdigit((unsigned char)c)) {
            int sline = L.line, scol = L.col;
            int64_t iv = 0;
            while (isdigit((unsigned char)*L.p)) {
                iv = iv * 10 + (*L.p - '0');
                L.p++; L.col++;
            }
            if (*L.p == '.' && isdigit((unsigned char)L.p[1])) {
                L.p++; L.col++;
                double fv = (double)iv;
                double scale = 0.1;
                while (isdigit((unsigned char)*L.p)) {
                    fv += (*L.p - '0') * scale;
                    scale *= 0.1;
                    L.p++; L.col++;
                }
                emit_float(&L, fv);
                tl->items[tl->len-1].line = sline;
                tl->items[tl->len-1].col  = scol;
            } else {
                emit_int(&L, iv);
                tl->items[tl->len-1].line = sline;
                tl->items[tl->len-1].col  = scol;
            }
            continue;
        }

        /* identifier / keyword */
        if (is_ident_start((unsigned char)c)) {
            int sline = L.line, scol = L.col;
            const char *start = L.p;
            while (is_ident_cont((unsigned char)*L.p)) { L.p++; L.col++; }
            size_t n = (size_t)(L.p - start);
            char *s = malloc(n+1);
            memcpy(s, start, n);
            s[n] = '\0';
            token_kind k = ident_kind(s, n);
            if (k == T_IDENT) {
                emit_str(&L, T_IDENT, s);
            } else {
                /* keywords don't carry text — save it anyway for diagnostics */
                emit_str(&L, k, s);
            }
            tl->items[tl->len-1].line = sline;
            tl->items[tl->len-1].col  = scol;
            continue;
        }

        /* .. range */
        if (c == '.' && L.p[1] == '.') {
            int sline = L.line, scol = L.col;
            L.p += 2; L.col += 2;
            emit(&L, T_KW_DOTDOT);
            tl->items[tl->len-1].line = sline;
            tl->items[tl->len-1].col  = scol;
            continue;
        }

        /* operators — two-char first */
        int sline = L.line, scol = L.col;
        char c2 = L.p[1];
        if (c == '=' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_EQ); goto done_op; }
        if (c == '!' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_NE); goto done_op; }
        if (c == '<' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_LE); goto done_op; }
        if (c == '>' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_GE); goto done_op; }
        if (c == '&' && c2 == '&') { L.p+=2; L.col+=2; emit(&L, T_AND); goto done_op; }
        if (c == '|' && c2 == '|') { L.p+=2; L.col+=2; emit(&L, T_OR); goto done_op; }
        if (c == '<' && c2 == '<') { L.p+=2; L.col+=2; emit(&L, T_SHL); goto done_op; }
        if (c == '>' && c2 == '>') { L.p+=2; L.col+=2; emit(&L, T_SHR); goto done_op; }
        /* compound assignment */
        if (c == '+' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_PLUS_EQ); goto done_op; }
        if (c == '-' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_MINUS_EQ); goto done_op; }
        if (c == '*' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_STAR_EQ); goto done_op; }
        if (c == '/' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_SLASH_EQ); goto done_op; }
        if (c == '%' && c2 == '=') { L.p+=2; L.col+=2; emit(&L, T_PERCENT_EQ); goto done_op; }

        /* single-char */
        switch (c) {
            case '+': emit(&L, T_PLUS); break;
            case '-': emit(&L, T_MINUS); break;
            case '*': emit(&L, T_STAR); break;
            case '/': emit(&L, T_SLASH); break;
            case '%': emit(&L, T_PERCENT); break;
            case '^': emit(&L, T_CARET); break;
            case '=': emit(&L, T_ASSIGN); break;
            case '<':
                /* At statement start: block-open. Otherwise: less-than operator.
                 * But also: if we already emitted a T_ANGLE_O on this logical
                 * line that hasn't been closed, the next < is comparison. We
                 * approximate: < at statement start = block open, else = LT.
                 * The parser still disambiguates within parens. */
                if (L.at_stmt_start) emit(&L, T_ANGLE_O);
                else emit(&L, T_LT);
                break;
            case '>':
                /* > is always GT (comparison) unless... we need a way to close
                 * a guard block. We'll emit T_ANGLE_C only if we're "inside"
                 * a guard header. But the lexer doesn't track that.
                 *
                 * Compromise: emit T_GT always. The parser will treat T_GT as
                 * both comparison and guard-close based on angle_depth. */
                emit(&L, T_GT);
                break;
            case '!': emit(&L, T_BANG); break;
            case '&': emit(&L, T_AMP); break;
            case '|': emit(&L, T_PIPE); break;
            case '~': emit(&L, T_TILDE); break;
            case ',': emit(&L, T_COMMA); break;
            case '?': emit(&L, T_QUESTION); break;
            case ';': emit(&L, T_SEMI); break;
            case '[': {
                /* Check for [langname] foreign block at statement start.
                 * Pattern: [ ident ] at start of a line. If ident is a
                 * known foreign language, capture the raw body. */
                if (L.at_stmt_start) {
                    /* Look ahead: [ ident ] */
                    const char *q = L.p + 1;
                    while (*q == ' ' || *q == '\t') q++;
                    if (is_ident_start((unsigned char)*q)) {
                        const char *name_start = q;
                        while (is_ident_cont((unsigned char)*q)) q++;
                        size_t name_len = (size_t)(q - name_start);
                        char *name = malloc(name_len + 1);
                        memcpy(name, name_start, name_len);
                        name[name_len] = '\0';
                        const char *r = q;
                        while (*r == ' ' || *r == '\t') r++;
                        if (*r == ']') {
                            /* We have [ident]. Check if it's a foreign lang. */
                            if (is_foreign_lang(name)) {
                                /* Emit: T_SQUARE_O, T_IDENT(name), T_SQUARE_C,
                                 * T_RAW_STRING(body). Then continue lexing
                                 * from after the captured body. */
                                int opener_col = L.col;
                                emit(&L, T_SQUARE_O);
                                /* advance past [ */
                                L.p++; L.col++;
                                /* emit the ident */
                                emit_str(&L, T_IDENT, name);
                                /* advance past ident and any ws */
                                L.p = name_start + name_len;
                                L.col = opener_col + 1 + (int)(name_start - (L.p - name_len)) ;
                                L.col = opener_col + 1;
                                while (*L.p == ' ' || *L.p == '\t') { L.p++; L.col++; }
                                /* emit ] */
                                emit(&L, T_SQUARE_C);
                                L.p++; L.col++;  /* consume ] */
                                /* capture raw body */
                                int saved_col = L.col;
                                char *body = capture_raw_block(&L, opener_col);
                                (void)saved_col;
                                emit_str(&L, T_RAW_STRING, body);
                                /* Set at_stmt_start so the next [/(/{/< at
                                 * col 1 starts a new block (which it will,
                                 * because the body capture stopped at it). */
                                L.at_stmt_start = 1;
                                break;  /* done with this [ */
                            }
                        }
                        free(name);
                    }
                }
                emit(&L, T_SQUARE_O);
                break;
            }
            case ']': emit(&L, T_SQUARE_C); break;
            case '(': emit(&L, T_PAREN_O); break;
            case ')': emit(&L, T_PAREN_C); break;
            case '{': emit(&L, T_BRACE_O); break;
            case '}': emit(&L, T_BRACE_C); break;
            default:
                g_set_error("%s:%d:%d: unexpected character '%c' (0x%02x)",
                            L.srcname, L.line, L.col, c, (unsigned char)c);
                tok_free(tl);
                return NULL;
        }
        L.p++; L.col++;
        L.at_stmt_start = 0;
        done_op:
        tl->items[tl->len-1].line = sline;
        tl->items[tl->len-1].col  = scol;
        L.at_stmt_start = 0;
    }

    emit(&L, T_EOF);
    tl->nlines = L.line;
    return tl;
}

void tok_free(tokenlist *tl) {
    if (!tl) return;
    for (size_t i = 0; i < tl->len; i++) {
        free(tl->items[i].text);
    }
    free(tl->items);
    free(tl);
}

const char *tok_kind_name(token_kind k) {
    switch (k) {
        case T_EOF: return "EOF";
        case T_NEWLINE: return "NEWLINE";
        case T_SEMI: return "SEMI";
        case T_IDENT: return "IDENT";
        case T_INT: return "INT";
        case T_FLOAT: return "FLOAT";
        case T_STRING: return "STRING";
        case T_RAW_STRING: return "RAW_STRING";
        case T_SQUARE_O: return "[";
        case T_SQUARE_C: return "]";
        case T_PAREN_O: return "(";
        case T_PAREN_C: return ")";
        case T_BRACE_O: return "{";
        case T_BRACE_C: return "}";
        case T_ANGLE_O: return "<";
        case T_ANGLE_C: return ">";
        case T_KW_RETURN: return "return";
        case T_KW_STOP: return "stop";
        case T_KW_NEXT: return "next";
        case T_KW_RAISE: return "raise";
        case T_KW_PRINT: return "print";
        case T_KW_THE: return "the";
        case T_KW_IF: return "if";
        case T_KW_ELSE: return "else";
        case T_KW_ELIF: return "elif";
        case T_KW_WHEN: return "when";
        case T_KW_FOR: return "for";
        case T_KW_IN: return "in";
        case T_KW_DOTDOT: return "..";
        case T_KW_TRUE: return "true";
        case T_KW_FALSE: return "false";
        case T_KW_NIL: return "nil";
        case T_PLUS: return "+";
        case T_MINUS: return "-";
        case T_STAR: return "*";
        case T_SLASH: return "/";
        case T_PERCENT: return "%";
        case T_CARET: return "^";
        case T_ASSIGN: return "=";
        case T_EQ: return "==";
        case T_NE: return "!=";
        case T_LE: return "<=";
        case T_GE: return ">=";
        case T_PLUS_EQ: return "+=";
        case T_MINUS_EQ: return "-=";
        case T_STAR_EQ: return "*=";
        case T_SLASH_EQ: return "/=";
        case T_PERCENT_EQ: return "%=";
        case T_AND: return "&&";
        case T_OR: return "||";
        case T_BANG: return "!";
        case T_AMP: return "&";
        case T_PIPE: return "|";
        case T_TILDE: return "~";
        case T_SHL: return "<<";
        case T_SHR: return ">>";
        case T_LT: return "<";
        case T_GT: return ">";
        case T_COMMA: return ",";
        case T_QUESTION: return "?";
        case T_INDENT: return "INDENT";
        case T_DEDENT: return "DEDENT";
    }
    return "?";
}
