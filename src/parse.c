/* parse.c — Glyph parser: tokens -> AST.
 *
 * Grammar (informal):
 *
 *   program     := stmt*
 *   stmt        := block | simple | ctrl
 *   block       := '[' name params? ']' body
 *                | '(' (count | name | 'for' id 'in' expr '..' expr (step expr)?) ')' body
 *                | '<' ('if' | 'when') cond_body (else body)? '>'
 *                | '{' name '}' body
 *   simple      := 'return' expr? | 'stop' | 'next' | 'raise' id
 *                | 'print' expr (',' expr)*
 *                | 'the' '[' id ']'        -- invoke squire
 *                | assignment | expr
 *   assignment  := id '=' expr
 *   expr        := or_expr
 *   or_expr     := and_expr ('||' and_expr)*
 *   ... (precedence as per spec)
 *
 * Block bodies: after a closing ']' / ')' / '>' / '}' of the block *header*,
 * the parser collects statements until the matching closing delimiter is
 * found at the same paren-depth. We use the explicit close tokens for shape
 * blocks: blocks end with the appropriate close token at the top level
 * of the block body.
 *
 * IMPORTANT: in Glyph, a shape's opening bracket starts the header; the body
 * follows; the matching close bracket ends the block. So:
 *
 *     [name]
 *       ...body...
 *
 * The closing ] is implicit at the next stmt at lower-or-equal bracket level.
 * Actually we use explicit close brackets to terminate blocks. Re-reading:
 *
 * To avoid ambiguity, we adopt a hybrid rule:
 *   - The bracket that opens a block ('[' '(' '<' '{') ALSO requires an
 *     explicit matching close at the start of a later line, at the same
 *     "statement-start" position. Whitespace/indentation is ignored.
 *   - This is the simplest, most predictable rule: every open has a close.
 *
 * That gives:
 *
 *     [squire]
 *       x = 1
 *       x = x + 1
 *     []            <- explicit close
 *
 * To make this less ugly we ALSO accept the form where the close is the bare
 * closing bracket on its own line. The body terminates when we encounter
 * the matching close at statement start.
 */

#include "glyph.h"
#include "platform.h"

typedef struct {
    const tokenlist *tl;
    size_t           i;
    int              err;
    int              angle_depth;   /* >0 means we're inside a <...> guard block; treat > as closer, not operator */
} parser;

static a_node *parse_stmt(parser *P);
static a_node *parse_expr(parser *P);
static a_node *parse_block(parser *P);
static a_node *parse_program(parser *P);

/* Some keywords (next, stop, raise, print, the) can also be used as variable
 * names when the context makes it unambiguous. This returns non-zero if the
 * token kind is usable as an identifier. */
static int is_ident_like(token_kind k) {
    return k == T_IDENT || k == T_KW_NEXT || k == T_KW_STOP ||
           k == T_KW_RAISE || k == T_KW_PRINT || k == T_KW_THE;
}

/* ------------------------------------------------------------------ */

static const token *cur(parser *P) {
    return &P->tl->items[P->i];
}
static const token *peek(parser *P, int off) {
    size_t j = P->i + off;
    if (j >= P->tl->len) j = P->tl->len - 1;
    return &P->tl->items[j];
}
static int accept(parser *P, token_kind k) {
    if (cur(P)->kind == k) { P->i++; return 1; }
    return 0;
}
static int expect(parser *P, token_kind k) {
    if (cur(P)->kind == k) { P->i++; return 1; }
    const token *t = cur(P);
    g_set_error("%s:%d:%d: expected %s but got %s%s%s",
                P->tl->srcname, t->line, t->col,
                tok_kind_name(k),
                t->text ? "'" : "",
                t->text ? t->text : tok_kind_name(t->kind),
                t->text ? "'" : "");
    P->err = 1;
    return 0;
}

static void skip_newlines(parser *P) {
    while (cur(P)->kind == T_NEWLINE || cur(P)->kind == T_SEMI) P->i++;
}

/* parse a list of statements. Stops when:
 *   - the current token's column is <= min_col (dedent), OR
 *   - the current token kind is in stoppers[], OR
 *   - EOF.
 * returns a list node whose children are the statements.
 */
static a_node *parse_stmt_list_until(parser *P, int min_col, const token_kind *stoppers, int nstop) {
    a_node *list = a_new(A_PROGRAM, cur(P)->line, cur(P)->col);
    while (1) {
        skip_newlines(P);
        token_kind k = cur(P)->kind;
        int stop = 0;
        for (int i = 0; i < nstop; i++) if (k == stoppers[i]) { stop = 1; break; }
        if (stop || k == T_EOF) break;
        /* dedent check: if current token is at col <= min_col, body ends.
         * min_col is the opener's column; any dedent (col <= opener_col) ends the body. */
        if (cur(P)->col <= min_col) break;
        a_node *s = parse_stmt(P);
        if (!s || P->err) { a_free(list); return NULL; }
        a_addchild(list, s);
    }
    return list;
}

/* parse one statement */
static a_node *parse_stmt(parser *P) {
    const token *t = cur(P);
    int line = t->line, col = t->col;

    switch (t->kind) {
        /* ---- shape blocks ---- */
        case T_SQUARE_O: case T_PAREN_O: case T_ANGLE_O: case T_BRACE_O:
            return parse_block(P);

        /* ---- ? shorthand for print ---- */
        case T_QUESTION: {
            P->i++;
            a_node *n = a_new(A_PRINT, line, col);
            do {
                a_node *e = parse_expr(P);
                if (!e || P->err) { a_free(n); return NULL; }
                a_addchild(n, e);
            } while (accept(P, T_COMMA));
            return n;
        }

        /* ---- control keywords ---- */
        case T_KW_RETURN: {
            P->i++;
            a_node *n = a_new(A_RETURN, line, col);
            /* optional expr; if next is newline/semi/etc, return nil */
            token_kind k = cur(P)->kind;
            if (k != T_NEWLINE && k != T_SEMI && k != T_EOF &&
                k != T_SQUARE_C && k != T_PAREN_C && k != T_GT && k != T_BRACE_C) {
                n->lhs = parse_expr(P);
                if (P->err) { a_free(n); return NULL; }
            }
            return n;
        }
        case T_KW_STOP:  P->i++; return a_new(A_STOP, line, col);
        case T_KW_NEXT:  P->i++; return a_new(A_NEXT, line, col);
        case T_KW_RAISE: {
            P->i++;
            const token *id = cur(P);
            if (id->kind != T_IDENT) {
                g_set_error("%s:%d:%d: raise expects a trigger name",
                            P->tl->srcname, id->line, id->col);
                P->err = 1; return NULL;
            }
            a_node *n = a_new(A_RAISE, line, col);
            n->sval = strdup(id->text);
            P->i++;
            return n;
        }
        case T_KW_PRINT: {
            P->i++;
            a_node *n = a_new(A_PRINT, line, col);
            do {
                a_node *e = parse_expr(P);
                if (!e || P->err) { a_free(n); return NULL; }
                a_addchild(n, e);
            } while (accept(P, T_COMMA));
            return n;
        }
        case T_KW_THE: {
            P->i++;
            if (!expect(P, T_SQUARE_O)) return NULL;
            const token *id = cur(P);
            if (id->kind != T_IDENT) {
                g_set_error("%s:%d:%d: 'the' expects [squirename]", P->tl->srcname, id->line, id->col);
                P->err = 1; return NULL;
            }
            a_node *inv = a_new(A_INVOKE, line, col);
            inv->sval = strdup(id->text);
            P->i++;
            while (cur(P)->kind != T_SQUARE_C && cur(P)->kind != T_EOF &&
                   cur(P)->kind != T_NEWLINE) {
                a_node *a = parse_expr(P);
                if (!a || P->err) { a_free(inv); return NULL; }
                a_addchild(inv, a);
                accept(P, T_COMMA);
            }
            if (!expect(P, T_SQUARE_C)) { a_free(inv); return NULL; }
            return inv;
        }

        /* ---- assignment or expression ---- */
        case T_IDENT: {
            /* lookahead: id followed by '=' is assignment */
            if (peek(P, 1)->kind == T_ASSIGN) {
                a_node *n = a_new(A_ASSIGN, line, col);
                n->sval = strdup(t->text);
                P->i += 2;  /* skip ident and = */
                n->rhs = parse_expr(P);
                if (P->err) { a_free(n); return NULL; }
                return n;
            }
            /* compound assignment: += -= *= /= %= */
            token_kind nk = peek(P, 1)->kind;
            if (nk == T_PLUS_EQ || nk == T_MINUS_EQ || nk == T_STAR_EQ ||
                nk == T_SLASH_EQ || nk == T_PERCENT_EQ) {
                const char *op;
                switch (nk) {
                    case T_PLUS_EQ:    op = "+"; break;
                    case T_MINUS_EQ:   op = "-"; break;
                    case T_STAR_EQ:    op = "*"; break;
                    case T_SLASH_EQ:   op = "/"; break;
                    case T_PERCENT_EQ: op = "%"; break;
                    default: op = "+"; break;
                }
                a_node *n = a_new(A_COMPOUND_ASSIGN, line, col);
                n->sval = strdup(op);
                /* store var name in lhs as an IDENT node */
                n->lhs = a_new(A_IDENT, line, col);
                n->lhs->sval = strdup(t->text);
                P->i += 2;  /* skip ident and op= */
                n->rhs = parse_expr(P);
                if (P->err) { a_free(n); return NULL; }
                return n;
            }
            /* lookahead: id '[' ... ']' '=' is index assignment */
            if (peek(P, 1)->kind == T_SQUARE_O) {
                /* scan forward for matching ] followed by = */
                size_t saved = P->i;
                P->i += 2;  /* skip ident and '[' */
                a_node *idx = parse_expr(P);
                if (P->err) { return NULL; }
                if (cur(P)->kind == T_SQUARE_C) {
                    P->i++;  /* skip ] */
                    if (cur(P)->kind == T_ASSIGN) {
                        P->i++;  /* skip = */
                        a_node *n = a_new(A_INDEX_ASSIGN, line, col);
                        n->sval = strdup(t->text);
                        n->lhs = idx;
                        n->rhs = parse_expr(P);
                        if (P->err) { a_free(n); return NULL; }
                        return n;
                    }
                }
                /* not an index assignment; rewind and parse as expression */
                P->i = saved;
            }
            /* fallthrough to expr */
        }
        /* fallthrough */
        default: {
            a_node *e = parse_expr(P);
            if (P->err) { a_free(e); return NULL; }
            return e;
        }
    }
}

/* parse a shape block (squire / loop / guard / trigger) */
static a_node *parse_block(parser *P) {
    const token *t = cur(P);
    int line = t->line, col = t->col;
    token_kind open = t->kind;
    a_kind    bkind;

    switch (open) {
        case T_SQUARE_O: bkind = A_BLOCK_SQUIRE;  break;
        case T_PAREN_O:  bkind = A_BLOCK_LOOP_INF; break;
        case T_ANGLE_O:  bkind = A_BLOCK_GUARD;    break;
        case T_BRACE_O:  bkind = A_BLOCK_TRIGGER;  break;
        default:
            g_set_error("%s:%d:%d: internal: parse_block on non-bracket", P->tl->srcname, line, col);
            P->err = 1; return NULL;
    }

    P->i++; /* consume open bracket */

    a_node *blk = a_new(bkind, line, col);

    if (open == T_SQUARE_O) {
        /* [name params...]
         * first token must be identifier (the name) */
        const token *id = cur(P);
        if (id->kind != T_IDENT) {
            g_set_error("%s:%d:%d: squire must start with a name", P->tl->srcname, id->line, id->col);
            P->err = 1; a_free(blk); return NULL;
        }
        blk->sval = strdup(id->text);
        P->i++;
        /* params: any subsequent identifier-like tokens (including keywords
         * used as names like 'next', 'stop') until ] */
        while (cur(P)->kind == T_IDENT || cur(P)->kind == T_KW_NEXT ||
               cur(P)->kind == T_KW_STOP || cur(P)->kind == T_KW_RAISE ||
               cur(P)->kind == T_KW_PRINT || cur(P)->kind == T_KW_THE) {
            a_node *p = a_new(A_IDENT, cur(P)->line, cur(P)->col);
            p->sval = strdup(cur(P)->text);
            a_addchild(blk, p);
            P->i++;
        }
        if (!expect(P, T_SQUARE_C)) { a_free(blk); return NULL; }

        /* Foreign-language block: [langname] followed by T_RAW_STRING.
         * The lexer already captured the raw body for us. */
        if (cur(P)->kind == T_RAW_STRING) {
            blk->kind = A_BLOCK_LANG;
            /* Store the raw body in the secondary payload (lhs is free
             * here — it's only used by A_BINOP/A_ASSIGN/A_RETURN). We
             * use a string literal node to hold it. */
            a_node *body = a_new(A_STRING, cur(P)->line, cur(P)->col);
            body->sval = strdup(cur(P)->text ? cur(P)->text : "");
            blk->lhs = body;
            P->i++;
            return blk;
        }

        /* body: statements with col > opener col, OR until explicit ] */
        token_kind stoppers[] = { T_SQUARE_C };
        a_node *body = parse_stmt_list_until(P, col, stoppers, 1);
        if (!body) { a_free(blk); return NULL; }
        for (size_t i = 0; i < body->nchild; i++)
            a_addchild(blk, body->children[i]);
        body->nchild = 0; a_free(body);
        /* optional explicit close */
        accept(P, T_SQUARE_C);
        return blk;
    }

    if (open == T_PAREN_O) {
        /* Three loop forms:
         *   (name)              — infinite loop
         *   (N)                 — N iterations
         *   (for i in a .. b [step s]) — range loop
         */
        if (cur(P)->kind == T_KW_FOR) {
            P->i++;
            const token *id = cur(P);
            if (id->kind != T_IDENT) {
                g_set_error("%s:%d:%d: 'for' expects a loop variable", P->tl->srcname, id->line, id->col);
                P->err = 1; a_free(blk); return NULL;
            }
            blk->kind = A_BLOCK_LOOP_FOR;
            blk->loop_var = strdup(id->text);
            P->i++;
            if (!expect(P, T_KW_IN)) { a_free(blk); return NULL; }
            blk->range_start = parse_expr(P);
            if (P->err) { a_free(blk); return NULL; }
            if (accept(P, T_KW_DOTDOT)) {
                /* range form: for i in a .. b [, step] */
                blk->range_end = parse_expr(P);
                if (P->err) { a_free(blk); return NULL; }
                if (accept(P, T_COMMA)) {
                    blk->range_step = parse_expr(P);
                    if (P->err) { a_free(blk); return NULL; }
                }
            } else {
                /* array iteration form: for x in array */
                blk->range_end = NULL;
            }
            if (!expect(P, T_PAREN_C)) { a_free(blk); return NULL; }
        }
        else if (cur(P)->kind == T_INT) {
            blk->kind = A_BLOCK_LOOP_COUNT;
            blk->cond = a_new(A_INT, cur(P)->line, cur(P)->col);
            blk->cond->ival = cur(P)->ival;
            P->i++;
            if (!expect(P, T_PAREN_C)) { a_free(blk); return NULL; }
        }
        else if (cur(P)->kind == T_IDENT) {
            /* (name): if name == "loop" → infinite loop;
             * otherwise → labeled sequence block (run once).
             * This matches the user's spec where (main) is the entry point
             * and (loop) is an infinite loop. */
            const char *nm = cur(P)->text;
            if (strcmp(nm, "loop") == 0) {
                blk->kind = A_BLOCK_LOOP_INF;
            } else {
                blk->kind = A_BLOCK_SEQ;
            }
            blk->sval = strdup(nm);
            P->i++;
            if (!expect(P, T_PAREN_C)) { a_free(blk); return NULL; }
        }
        else {
            g_set_error("%s:%d:%d: '()' must contain a name, an integer, or 'for ...'",
                        P->tl->srcname, cur(P)->line, cur(P)->col);
            P->err = 1; a_free(blk); return NULL;
        }

        token_kind stoppers[] = { T_PAREN_C };
        a_node *body = parse_stmt_list_until(P, col, stoppers, 1);
        if (!body) { a_free(blk); return NULL; }
        for (size_t i = 0; i < body->nchild; i++)
            a_addchild(blk, body->children[i]);
        body->nchild = 0; a_free(body);
        accept(P, T_PAREN_C);
        return blk;
    }

    if (open == T_ANGLE_O) {
        /* <if cond> ... [elif cond2> ... ]* [else ...]
         * <when cond> ...
         * The guard close `>` is T_GT. 'elif' and 'else' are handled
         * recursively to support unlimited chaining.
         */
        if (cur(P)->kind == T_KW_IF || cur(P)->kind == T_KW_WHEN) {
            P->i++;
        }
        P->angle_depth++;
        blk->cond = parse_expr(P);
        P->angle_depth--;
        if (P->err) { a_free(blk); return NULL; }
        if (!expect(P, T_GT)) { a_free(blk); return NULL; }

        /* then-body: indented statements until dedent OR 'else'/'elif' OR explicit '>' */
        token_kind stoppers[] = { T_GT, T_KW_ELSE, T_KW_ELIF };
        a_node *body = parse_stmt_list_until(P, col, stoppers, 3);
        if (!body) { a_free(blk); return NULL; }
        for (size_t i = 0; i < body->nchild; i++)
            a_addchild(blk, body->children[i]);
        body->nchild = 0; a_free(body);

        /* Handle elif chain recursively */
        if (cur(P)->kind == T_KW_ELIF) {
            P->i++;  /* consume 'elif' */
            a_node *first_elif = a_new(A_BLOCK_GUARD, cur(P)->line, cur(P)->col);
            a_node *elif_blk = first_elif;
            if (cur(P)->kind == T_KW_IF) P->i++;  /* optional 'if' */
            P->angle_depth++;
            elif_blk->cond = parse_expr(P);
            P->angle_depth--;
            if (P->err) { a_free(blk); a_free(elif_blk); return NULL; }
            if (!expect(P, T_GT)) { a_free(blk); a_free(elif_blk); return NULL; }
            /* parse elif body */
            token_kind es[] = { T_GT, T_KW_ELSE, T_KW_ELIF };
            a_node *eb = parse_stmt_list_until(P, col, es, 3);
            if (!eb) { a_free(blk); a_free(elif_blk); return NULL; }
            for (size_t i = 0; i < eb->nchild; i++)
                a_addchild(elif_blk, eb->children[i]);
            eb->nchild = 0; a_free(eb);

            /* Handle further elif chains */
            while (cur(P)->kind == T_KW_ELIF) {
                P->i++;
                a_node *next = a_new(A_BLOCK_GUARD, cur(P)->line, cur(P)->col);
                if (cur(P)->kind == T_KW_IF) P->i++;
                P->angle_depth++;
                next->cond = parse_expr(P);
                P->angle_depth--;
                if (P->err) { a_free(blk); return NULL; }
                if (!expect(P, T_GT)) { a_free(blk); return NULL; }
                token_kind ns[] = { T_GT, T_KW_ELSE, T_KW_ELIF };
                a_node *nb = parse_stmt_list_until(P, col, ns, 3);
                if (!nb) { a_free(blk); return NULL; }
                for (size_t i = 0; i < nb->nchild; i++)
                    a_addchild(next, nb->children[i]);
                nb->nchild = 0; a_free(nb);
                /* set current elif's else_branch to next */
                a_node *wrap = a_new(A_PROGRAM, next->line, next->col);
                a_addchild(wrap, next);
                elif_blk->else_branch = wrap;
                elif_blk = next;
            }
            if (cur(P)->kind == T_KW_ELSE) {
                P->i++;
                token_kind es2[] = { T_GT };
                a_node *elsebody = parse_stmt_list_until(P, col, es2, 1);
                if (!elsebody) { a_free(blk); return NULL; }
                elif_blk->else_branch = elsebody;
            }
            /* wrap the FIRST elif as the else_branch of blk */
            a_node *wrap = a_new(A_PROGRAM, first_elif->line, first_elif->col);
            a_addchild(wrap, first_elif);
            blk->else_branch = wrap;
        } else if (cur(P)->kind == T_KW_ELSE) {
            P->i++;
            token_kind stop2[] = { T_GT };
            a_node *ebody = parse_stmt_list_until(P, col, stop2, 1);
            if (!ebody) { a_free(blk); return NULL; }
            blk->else_branch = ebody;
        }
        accept(P, T_GT);
        return blk;
    }

    if (open == T_BRACE_O) {
        /* {name} — trigger */
        const token *id = cur(P);
        if (id->kind != T_IDENT) {
            g_set_error("%s:%d:%d: trigger must start with a name", P->tl->srcname, id->line, id->col);
            P->err = 1; a_free(blk); return NULL;
        }
        blk->sval = strdup(id->text);
        P->i++;
        if (!expect(P, T_BRACE_C)) { a_free(blk); return NULL; }
        token_kind stoppers[] = { T_BRACE_C };
        a_node *body = parse_stmt_list_until(P, col, stoppers, 1);
        if (!body) { a_free(blk); return NULL; }
        for (size_t i = 0; i < body->nchild; i++)
            a_addchild(blk, body->children[i]);
        body->nchild = 0; a_free(body);
        accept(P, T_BRACE_C);
        return blk;
    }

    /* unreachable */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* expressions (Pratt-style by hand)                                  */
/* ------------------------------------------------------------------ */

static a_node *parse_primary(parser *P);
static a_node *parse_unary(parser *P);
static a_node *parse_pow(parser *P);
static a_node *parse_mul(parser *P);
static a_node *parse_add(parser *P);
static a_node *parse_shift(parser *P);
static a_node *parse_cmp(parser *P);
static a_node *parse_not(parser *P);
static a_node *parse_and(parser *P);
static a_node *parse_band(parser *P);
static a_node *parse_bxor(parser *P);
static a_node *parse_bor(parser *P);
static a_node *parse_or(parser *P);

static a_node *parse_expr(parser *P) {
    return parse_or(P);
}

static a_node *make_binop(const char *op, a_node *l, a_node *r, int line, int col) {
    a_node *n = a_new(A_BINOP, line, col);
    n->sval = strdup(op);
    n->lhs = l;
    n->rhs = r;
    return n;
}

static a_node *parse_primary(parser *P) {
    const token *t = cur(P);
    int line = t->line, col = t->col;

    switch (t->kind) {
        case T_INT: {
            a_node *n = a_new(A_INT, line, col);
            n->ival = t->ival;
            P->i++;
            return n;
        }
        case T_FLOAT: {
            a_node *n = a_new(A_FLOAT, line, col);
            n->fval = t->fval;
            P->i++;
            return n;
        }
        case T_STRING: {
            /* Check for string interpolation: #{expr}
             * If the string contains #{, we build a concatenation chain.
             * "x=#{y}z" becomes "x=" + string(y) + "z"
             * This is the #1 simplification for developers — no more
             * verbose "x = " + string(x) patterns. */
            const char *s = t->text;
            const char *p = s;
            const char *interp = strstr(p, "#{");
            if (!interp) {
                /* No interpolation — plain string */
                a_node *n = a_new(A_STRING, line, col);
                n->sval = strdup(t->text);
                P->i++;
                return n;
            }
            /* Has interpolation — build concatenation chain */
            a_node *result = NULL;
            while (1) {
                const char *interp = strstr(p, "#{");
                if (!interp) {
                    /* Rest is a literal string */
                    if (interp != p || *p) {
                        a_node *lit = a_new(A_STRING, line, col);
                        lit->sval = strdup(p);
                        if (!result) result = lit;
                        else result = make_binop("+", result, lit, line, col);
                    }
                    break;
                }
                /* Literal part before #{ */
                if (interp > p) {
                    size_t len = interp - p;
                    a_node *lit = a_new(A_STRING, line, col);
                    lit->sval = malloc(len + 1);
                    memcpy(lit->sval, p, len);
                    lit->sval[len] = '\0';
                    if (!result) result = lit;
                    else result = make_binop("+", result, lit, line, col);
                }
                /* Find the closing } */
                const char *close = strchr(interp + 2, '}');
                if (!close) {
                    g_set_error("%s:%d: unterminated #{ in string", P->tl->srcname, line);
                    P->err = 1;
                    return NULL;
                }
                /* Extract the expression string */
                size_t expr_len = close - (interp + 2);
                char *expr_str = malloc(expr_len + 1);
                memcpy(expr_str, interp + 2, expr_len);
                expr_str[expr_len] = '\0';
                /* Lex and parse the expression */
                tokenlist *etl = lex(expr_str, P->tl->srcname);
                if (etl) {
                    parser ep = { .tl = etl, .i = 0, .err = 0, .angle_depth = 0 };
                    a_node *expr = parse_or(&ep);
                    if (!ep.err && expr) {
                        /* Wrap in string() call to auto-convert */
                        a_node *strcall = a_new(A_CALL, line, col);
                        strcall->sval = strdup("string");
                        a_addchild(strcall, expr);
                        if (!result) result = strcall;
                        else result = make_binop("+", result, strcall, line, col);
                    }
                    tok_free(etl);
                }
                free(expr_str);
                p = close + 1;
            }
            P->i++;
            return result ? result : a_new(A_STRING, line, col);
        }
        case T_KW_TRUE:  { a_node *n = a_new(A_BOOL, line, col); n->ival = 1; P->i++; return n; }
        case T_KW_FALSE: { a_node *n = a_new(A_BOOL, line, col); n->ival = 0; P->i++; return n; }
        case T_KW_NIL:   { a_node *n = a_new(A_NIL,  line, col); P->i++; return n; }

        case T_KW_THE: {
            /* 'the [name args...]' — squire invocation as an expression.
             * Args are space-separated (commas optional). */
            P->i++;
            if (!expect(P, T_SQUARE_O)) return NULL;
            const token *id = cur(P);
            if (id->kind != T_IDENT) {
                g_set_error("%s:%d:%d: 'the' expects [squirename]", P->tl->srcname, id->line, id->col);
                P->err = 1; return NULL;
            }
            a_node *inv = a_new(A_INVOKE, line, col);
            inv->sval = strdup(id->text);
            P->i++;
            while (cur(P)->kind != T_SQUARE_C && cur(P)->kind != T_EOF &&
                   cur(P)->kind != T_NEWLINE) {
                a_node *a = parse_expr(P);
                if (!a || P->err) { a_free(inv); return NULL; }
                a_addchild(inv, a);
                /* accept optional comma */
                accept(P, T_COMMA);
            }
            if (!expect(P, T_SQUARE_C)) { a_free(inv); return NULL; }
            return inv;
        }

        case T_IDENT:
        case T_KW_NEXT:
        case T_KW_STOP:
        case T_KW_RAISE:
        case T_KW_PRINT: {
            /* could be: variable, function call (followed by '('), or squire invocation.
             * Some keywords (next, stop, raise, print) can also be used as variable
             * names in expression position. */
            a_node *n = a_new(A_IDENT, line, col);
            n->sval = strdup(t->text);
            P->i++;
            /* function call: ident '(' args ')' */
            if (cur(P)->kind == T_PAREN_O) {
                P->i++;
                n->kind = A_CALL;
                while (cur(P)->kind != T_PAREN_C && cur(P)->kind != T_EOF) {
                    a_node *a = parse_expr(P);
                    if (P->err) { a_free(n); return NULL; }
                    a_addchild(n, a);
                    if (!accept(P, T_COMMA)) break;
                }
                if (!expect(P, T_PAREN_C)) { a_free(n); return NULL; }
            }
            /* index: ident '[' expr ']' */
            while (cur(P)->kind == T_SQUARE_O) {
                P->i++;
                a_node *idx = parse_expr(P);
                if (P->err) { a_free(n); return NULL; }
                if (!expect(P, T_SQUARE_C)) { a_free(n); a_free(idx); return NULL; }
                a_node *ix = a_new(A_INDEX, line, col);
                ix->lhs = n;
                ix->rhs = idx;
                n = ix;
            }
            return n;
        }

        case T_PAREN_O: {
            /* grouping (NOT a loop here) */
            P->i++;
            int saved = P->angle_depth;
            P->angle_depth = 0;  /* inside parens, > is comparison not guard-close */
            a_node *e = parse_expr(P);
            P->angle_depth = saved;
            if (P->err) { a_free(e); return NULL; }
            if (!expect(P, T_PAREN_C)) { a_free(e); return NULL; }
            return e;
        }

        case T_SQUARE_O: {
            /* Could be:
             *   [name args...]  — squire invocation (if first token is IDENT
             *                     AND not followed by ',' or ']')
             *   [expr, expr, ...]  — array literal
             * Disambiguate: if first token after [ is IDENT and the token
             * after that is ']' or a non-comma expression continuation that
             * looks like an arg, treat as invoke. Otherwise array literal.
             *
             * Simpler rule: if first token after [ is IDENT followed by ']' or
             * by another IDENT/expr (space-separated args) or by ',', it's
             * ambiguous. We use: if first token is IDENT and second token is
             * ']' → invoke with no args. If first is IDENT and second is ','
             * → could be either (treat as array of identifiers). If first is
             * IDENT and second starts another expression → invoke.
             *
             * Cleanest: array literals use [a, b, c] with commas. Squire
             * invokes use [name arg1 arg2] with spaces (no commas) OR
             * [name] with no args. So: if there's a comma anywhere before
             * the matching ], it's an array literal. */
            P->i++;
            const token *id = cur(P);
            /* Look ahead for a comma before the matching ] at depth 0 */
            int has_comma = 0;
            int depth = 1;
            for (size_t j = P->i; j < P->tl->len && depth > 0; j++) {
                token_kind tk = P->tl->items[j].kind;
                if (tk == T_SQUARE_O || tk == T_PAREN_O || tk == T_BRACE_O) depth++;
                else if (tk == T_SQUARE_C || tk == T_PAREN_C || tk == T_BRACE_C) depth--;
                else if (tk == T_NEWLINE || tk == T_EOF) break;
                else if (depth == 1 && tk == T_COMMA) { has_comma = 1; break; }
            }

            if (id->kind == T_IDENT && !has_comma) {
                /* Could be squire invocation [name args...] or array literal
                 * starting with an identifier. Disambiguation rules:
                 * - [name] (IDENT followed immediately by ]) → array literal [name]
                 * - [name arg1 arg2 ...] (IDENT followed by more tokens) → squire invoke
                 * - [name(...)] (IDENT followed by () → array literal with func call
                 * - [name, ...] has comma → array literal (handled above)
                 *
                 * To explicitly invoke a squire with 0 args, use `the [name]`.
                 */
                token_kind after = peek(P, 1)->kind;
                int is_invoke = 0;
                if (after == T_SQUARE_C) {
                    /* [name] — array literal with one element (the variable name) */
                    is_invoke = 0;
                } else if (after == T_PAREN_O) {
                    /* [func_call(...)] — array literal */
                    is_invoke = 0;
                } else if (after == T_INT || after == T_FLOAT || after == T_STRING ||
                           after == T_IDENT || after == T_KW_TRUE || after == T_KW_FALSE ||
                           after == T_KW_NIL || after == T_MINUS || after == T_BANG ||
                           after == T_KW_THE) {
                    /* [name arg1 arg2 ...] — squire invoke */
                    is_invoke = 1;
                }
                /* Otherwise, treat as array literal */

                if (is_invoke) {
                    a_node *inv = a_new(A_INVOKE, line, col);
                    inv->sval = strdup(id->text);
                    P->i++;
                    while (cur(P)->kind != T_SQUARE_C && cur(P)->kind != T_EOF &&
                           cur(P)->kind != T_NEWLINE) {
                        a_node *a = parse_expr(P);
                        if (P->err) { a_free(inv); return NULL; }
                        a_addchild(inv, a);
                        accept(P, T_COMMA);
                    }
                    if (!expect(P, T_SQUARE_C)) { a_free(inv); return NULL; }
                    return inv;
                }
            }
            /* array literal [expr, expr, ...] */
            a_node *arr = a_new(A_ARRAY_LITERAL, line, col);
            while (cur(P)->kind != T_SQUARE_C && cur(P)->kind != T_EOF &&
                   cur(P)->kind != T_NEWLINE) {
                a_node *a = parse_expr(P);
                if (P->err) { a_free(arr); return NULL; }
                a_addchild(arr, a);
                if (!accept(P, T_COMMA)) break;
            }
            if (!expect(P, T_SQUARE_C)) { a_free(arr); return NULL; }
            return arr;
        }

        default:
            g_set_error("%s:%d:%d: unexpected token %s%s%s in expression",
                        P->tl->srcname, t->line, t->col,
                        t->text ? "" : tok_kind_name(t->kind),
                        t->text ? "'" : "",
                        t->text ? t->text : "");
            P->err = 1;
            return NULL;
    }
}

static a_node *parse_unary(parser *P) {
    const token *t = cur(P);
    if (t->kind == T_MINUS || t->kind == T_BANG) {
        int line = t->line, col = t->col;
        P->i++;
        a_node *operand = parse_unary(P);
        if (P->err) return NULL;
        a_node *n = a_new(A_UNOP, line, col);
        n->sval = strdup(t->kind == T_MINUS ? "-" : "!");
        n->lhs = operand;
        return n;
    }
    if (t->kind == T_PLUS) {
        P->i++;
        return parse_unary(P);
    }
    return parse_pow(P);
}

static a_node *parse_pow(parser *P) {
    a_node *l = parse_primary(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_CARET) {
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_unary(P); /* right-assoc */
        if (P->err) { a_free(l); return NULL; }
        l = make_binop("^", l, r, line, col);
    }
    return l;
}

static a_node *parse_mul(parser *P) {
    a_node *l = parse_unary(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_STAR || cur(P)->kind == T_SLASH || cur(P)->kind == T_PERCENT) {
        const char *op = cur(P)->kind == T_STAR ? "*" : (cur(P)->kind == T_SLASH ? "/" : "%");
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_unary(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop(op, l, r, line, col);
    }
    return l;
}

static a_node *parse_add(parser *P) {
    a_node *l = parse_mul(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_PLUS || cur(P)->kind == T_MINUS) {
        const char *op = cur(P)->kind == T_PLUS ? "+" : "-";
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_mul(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop(op, l, r, line, col);
    }
    return l;
}

static a_node *parse_shift(parser *P) {
    a_node *l = parse_add(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_SHL || cur(P)->kind == T_SHR) {
        const char *op = cur(P)->kind == T_SHL ? "<<" : ">>";
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_add(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop(op, l, r, line, col);
    }
    return l;
}

static a_node *parse_cmp(parser *P) {
    a_node *l = parse_shift(P);
    if (P->err) return NULL;
    while (1) {
        token_kind k = cur(P)->kind;
        /* Inside a <...> guard header (angle_depth > 0), a `>` could be either
         * a comparison operator OR the guard-close delimiter. We distinguish
         * by lookahead: if the token AFTER `>` can start an expression
         * (number, identifier, string, `(`, `-`, `!`), then `>` is a
         * comparison operator. Otherwise it's the guard close. */
        if (k == T_GT && P->angle_depth > 0) {
            token_kind next = peek(P, 1)->kind;
            int next_starts_expr = (next == T_INT || next == T_FLOAT ||
                next == T_STRING || next == T_IDENT || next == T_PAREN_O ||
                next == T_MINUS || next == T_BANG || next == T_KW_TRUE ||
                next == T_KW_FALSE || next == T_KW_NIL || next == T_KW_THE);
            if (!next_starts_expr) break;
            /* else: treat as comparison, fall through */
        }
        if (k != T_EQ && k != T_NE && k != T_LE && k != T_GE &&
            k != T_LT && k != T_GT) break;
        const char *op;
        switch (k) {
            case T_EQ: op = "=="; break;
            case T_NE: op = "!="; break;
            case T_LE: op = "<="; break;
            case T_GE: op = ">="; break;
            case T_LT: op = "<"; break;
            case T_GT: op = ">"; break;
            default: op = "?"; break;
        }
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_shift(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop(op, l, r, line, col);
    }
    return l;
}

static a_node *parse_not(parser *P) {
    /* `!` is unary and handled in parse_unary; `not` could be added later.
     * For precedence, && and || bind looser than <,>. We pass through to cmp. */
    return parse_cmp(P);
}

static a_node *parse_and(parser *P) {
    a_node *l = parse_not(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_AND) {
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_not(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop("&&", l, r, line, col);
    }
    return l;
}

static a_node *parse_band(parser *P) {
    a_node *l = parse_and(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_AMP) {
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_and(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop("&", l, r, line, col);
    }
    return l;
}

static a_node *parse_bxor(parser *P) {
    /* ^ is power in Glyph (handled in parse_pow). No bitwise xor operator in v1.
     * Users can use (a & ~b) | (~a & b) if needed. */
    return parse_band(P);
}

static a_node *parse_bor(parser *P) {
    a_node *l = parse_bxor(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_PIPE) {
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_bxor(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop("|", l, r, line, col);
    }
    return l;
}

static a_node *parse_or(parser *P) {
    a_node *l = parse_bor(P);
    if (P->err) return NULL;
    while (cur(P)->kind == T_OR) {
        int line = cur(P)->line, col = cur(P)->col;
        P->i++;
        a_node *r = parse_bor(P);
        if (P->err) { a_free(l); return NULL; }
        l = make_binop("||", l, r, line, col);
    }
    return l;
}

static a_node *parse_program(parser *P) {
    a_node *prog = a_new(A_PROGRAM, 1, 1);
    while (cur(P)->kind != T_EOF) {
        skip_newlines(P);
        if (cur(P)->kind == T_EOF) break;
        a_node *s = parse_stmt(P);
        if (!s || P->err) { a_free(prog); return NULL; }
        a_addchild(prog, s);
    }
    return prog;
}

a_node *parse(const tokenlist *tl) {
    parser P = { .tl = tl, .i = 0, .err = 0 };
    a_node *prog = parse_program(&P);
    if (P.err) { a_free(prog); return NULL; }
    return prog;
}
