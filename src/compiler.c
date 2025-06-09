#include "compiler.h"

typedef struct {
    size_t globals;

    SB     sb;
    size_t indent;
} Compiler;

static inline void sb_indent(Compiler *c) {
    for (size_t i = 0; i < c->indent; i++) {
        da_push(&c->sb, '\t');
    }
}

static_assert(COUNT_NODES == 7, "");
static void compile_expr(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 17, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            sb_sprintf(&c->sb, "%zuL", n->token.as.integer);
            break;

        case TOKEN_BOOL:
            sb_sprintf(&c->sb, "%dL", n->token.as.boolean);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 17, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            sb_sprintf(&c->sb, "-(");
            compile_expr(c, unary->operand);
            sb_sprintf(&c->sb, ")");
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 17, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            sb_sprintf(&c->sb, "(");
            compile_expr(c, binary->lhs);
            sb_sprintf(&c->sb, " + ");
            compile_expr(c, binary->rhs);
            sb_sprintf(&c->sb, ")");
            break;

        case TOKEN_SUB:
            sb_sprintf(&c->sb, "(");
            compile_expr(c, binary->lhs);
            sb_sprintf(&c->sb, " - ");
            compile_expr(c, binary->rhs);
            sb_sprintf(&c->sb, ")");
            break;

        case TOKEN_MUL:
            sb_sprintf(&c->sb, "(");
            compile_expr(c, binary->lhs);
            sb_sprintf(&c->sb, " * ");
            compile_expr(c, binary->rhs);
            sb_sprintf(&c->sb, ")");
            break;

        case TOKEN_DIV:
            sb_sprintf(&c->sb, "(");
            compile_expr(c, binary->lhs);
            sb_sprintf(&c->sb, " / ");
            compile_expr(c, binary->rhs);
            sb_sprintf(&c->sb, ")");
            break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 7, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        sb_sprintf(&c->sb, "if (");
        compile_expr(c, iff->condition);
        sb_sprintf(&c->sb, ") ");
        compile_stmt(c, iff->consequence);

        if (iff->antecedence) {
            sb_sprintf(&c->sb, " else ");
            compile_stmt(c, iff->antecedence);
        }
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        sb_sprintf(&c->sb, "{\n");
        c->indent++;
        for (Node *it = block->body.head; it; it = it->next) {
            sb_indent(c);
            compile_stmt(c, it);
            sb_sprintf(&c->sb, "\n");
        }
        c->indent--;
        sb_sprintf(&c->sb, "}");
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        n->data = ++c->globals;

        sb_sprintf(&c->sb, "void __glos_g%zu(void) ", n->data);
        compile_stmt(c, fn->body);
        sb_sprintf(&c->sb, "\n");
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        sb_sprintf(&c->sb, "printf(\"%%ld\\n\", (long) (");
        compile_expr(c, print->operand);
        sb_sprintf(&c->sb, "));");
    } break;

    default:
        compile_expr(c, n);
        break;
    }
}

void compile_nodes(Context *context, Cmd *cmd, const char *output) {
    Node *main = scope_find(context->globals, sv_from_cstr("main"));
    if (!main) {
        fprintf(stderr, "ERROR: Function 'main' is not defined\n");
        exit(1);
    }

    Compiler c = {0};
    sb_sprintf(&c.sb, "extern int printf(const char *fmt, ...);\n");

    for (size_t i = 0; i < context->globals.count; i++) {
        compile_stmt(&c, context->globals.data[i]);
    }

    sb_sprintf(&c.sb, "int main(void) {\n");
    c.indent++;
    sb_indent(&c);
    sb_sprintf(&c.sb, "__glos_g%zu();\n", main->data);
    sb_indent(&c);
    sb_sprintf(&c.sb, "return 0;\n");
    c.indent--;
    sb_sprintf(&c.sb, "}\n");

    da_push(cmd, "cc");
    da_push(cmd, "-g");
    da_push(cmd, "-o");
    da_push(cmd, output);
    da_push(cmd, "-x");
    da_push(cmd, "c");
    da_push(cmd, "-");

    FILE *in = NULL;
    Proc  proc = cmd_run_async(cmd, (CmdStdio) {.in = &in});

    if (in) {
        fwrite(c.sb.data, c.sb.count, 1, in);
        fclose(in);
    }

    const int code = cmd_wait(proc);
    if (code) {
        exit(code);
    }
}
