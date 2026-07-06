/* ast.c — AST node construction, freeing, dumping.
 */

#include "glyph.h"
#include "platform.h"

a_node *a_new(a_kind k, int line, int col) {
    a_node *n = calloc(1, sizeof(a_node));
    n->kind = k;
    n->line = line;
    n->col  = col;
    return n;
}

void a_free(a_node *n) {
    if (!n) return;
    for (size_t i = 0; i < n->nchild; i++) {
        a_free(n->children[i]);
    }
    free(n->children);
    free(n->sval);
    free(n->loop_var);
    /* lhs/rhs/cond/else_branch etc are referenced via children where applicable;
       nodes that store children separately (binop.lhs/rhs) own them. */
    free(n);
}

void a_addchild(a_node *parent, a_node *child) {
    if (!child) return;
    parent->children = realloc(parent->children, (parent->nchild+1) * sizeof(a_node*));
    parent->children[parent->nchild++] = child;
}

static const char *a_kind_name(a_kind k) {
    switch (k) {
        case A_INT: return "INT";
        case A_FLOAT: return "FLOAT";
        case A_STRING: return "STRING";
        case A_BOOL: return "BOOL";
        case A_NIL: return "NIL";
        case A_IDENT: return "IDENT";
        case A_BINOP: return "BINOP";
        case A_UNOP: return "UNOP";
        case A_CALL: return "CALL";
        case A_INDEX: return "INDEX";
        case A_ARRAY_LITERAL: return "ARRAY_LITERAL";
        case A_ASSIGN: return "ASSIGN";
        case A_COMPOUND_ASSIGN: return "COMPOUND_ASSIGN";
        case A_INDEX_ASSIGN: return "INDEX_ASSIGN";
        case A_PRINT: return "PRINT";
        case A_RETURN: return "RETURN";
        case A_STOP: return "STOP";
        case A_NEXT: return "NEXT";
        case A_RAISE: return "RAISE";
        case A_INVOKE: return "INVOKE";
        case A_BLOCK_SQUIRE: return "BLOCK_SQUIRE";
        case A_BLOCK_LOOP_COUNT: return "LOOP_COUNT";
        case A_BLOCK_LOOP_INF: return "LOOP_INF";
        case A_BLOCK_LOOP_FOR: return "LOOP_FOR";
        case A_BLOCK_SEQ: return "SEQ";
        case A_BLOCK_GUARD: return "GUARD";
        case A_BLOCK_TRIGGER: return "TRIGGER";
        case A_PROGRAM: return "PROGRAM";
    }
    return "?";
}

static void a_dump_r(FILE *out, a_node *n, int depth) {
    if (!n) { fprintf(out, "%*s<null>\n", depth*2, ""); return; }
    fprintf(out, "%*s(%d:%d) %s", depth*2, "", n->line, n->col, a_kind_name(n->kind));
    if (n->sval) fprintf(out, " %s", n->sval);
    if (n->kind == A_INT) fprintf(out, " %ld", (long)n->ival);
    if (n->kind == A_FLOAT) fprintf(out, " %g", n->fval);
    if (n->kind == A_BOOL) fprintf(out, " %s", n->ival ? "true" : "false");
    fprintf(out, "\n");
    if (n->lhs) a_dump_r(out, n->lhs, depth+1);
    if (n->rhs) a_dump_r(out, n->rhs, depth+1);
    if (n->cond) a_dump_r(out, n->cond, depth+1);
    if (n->else_branch) a_dump_r(out, n->else_branch, depth+1);
    if (n->range_start) { fprintf(out, "%*srange_start:\n", (depth+1)*2, ""); a_dump_r(out, n->range_start, depth+2); }
    if (n->range_end)   { fprintf(out, "%*srange_end:\n",   (depth+1)*2, ""); a_dump_r(out, n->range_end,   depth+2); }
    if (n->range_step)  { fprintf(out, "%*srange_step:\n",  (depth+1)*2, ""); a_dump_r(out, n->range_step,  depth+2); }
    for (size_t i = 0; i < n->nchild; i++) {
        a_dump_r(out, n->children[i], depth+1);
    }
}

void a_dump(FILE *out, a_node *n, int depth) {
    a_dump_r(out, n, depth);
}
