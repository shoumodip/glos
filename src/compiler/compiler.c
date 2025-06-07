#include "compiler.h"
#include "qbe/qbe.h"
#include <stdbool.h>

typedef struct {
    Qbe   *qbe;
    QbeFn *fn;
} Compiler;

static_assert(COUNT_TYPES == 2, "");
static void compile_type(Type *type) {
    switch (type->kind) {
    case TYPE_BOOL:
        type->qbe = qbe_type_basic(QBE_TYPE_I8);
        break;

    case TYPE_INT:
        type->qbe = qbe_type_basic(QBE_TYPE_I64);
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 4, "");
static QbeNode *compile_expr(Compiler *c, Node *n) {
    if (!n) {
        return NULL;
    }

    compile_type(&n->type);
    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 12, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            return qbe_atom_int(c->qbe, n->type.qbe.kind, n->token.as.integer);

        case TOKEN_BOOL:
            return qbe_atom_int(c->qbe, n->type.qbe.kind, n->token.as.boolean);

        default:
            unreachable();
        }
        break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 12, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            QbeNode *operand = compile_expr(c, unary->operand);
            return qbe_build_unary(c->qbe, c->fn, QBE_UNARY_NEG, n->type.qbe, operand);
        };

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 12, "");
        switch (n->token.kind) {
        case TOKEN_ADD: {
            QbeNode *lhs = compile_expr(c, binary->lhs);
            QbeNode *rhs = compile_expr(c, binary->rhs);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ADD, n->type.qbe, lhs, rhs);
        };

        case TOKEN_SUB: {
            QbeNode *lhs = compile_expr(c, binary->lhs);
            QbeNode *rhs = compile_expr(c, binary->rhs);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_SUB, n->type.qbe, lhs, rhs);
        };

        case TOKEN_MUL: {
            QbeNode *lhs = compile_expr(c, binary->lhs);
            QbeNode *rhs = compile_expr(c, binary->rhs);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_MUL, n->type.qbe, lhs, rhs);
        };

        case TOKEN_DIV: {
            QbeNode *lhs = compile_expr(c, binary->lhs);
            QbeNode *rhs = compile_expr(c, binary->rhs);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_SDIV, n->type.qbe, lhs, rhs);
        };

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 4, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        assert(print->operand->kind == NODE_BINARY);

        static QbeNode *fn = NULL;
        if (!fn) {
            fn = qbe_atom_symbol(c->qbe, qbe_sv_from_cstr("printf"), qbe_type_basic(QBE_TYPE_PTR));
        }

        static QbeNode *fmt = NULL;
        if (!fmt) {
            fmt = qbe_str_new(c->qbe, qbe_sv_from_cstr("%ld\n"));
        }

        QbeCall *call = qbe_build_call(c->qbe, c->fn, fn, qbe_type_basic(QBE_TYPE_I32));
        qbe_call_add_arg(c->qbe, call, fmt);

        QbeNode *operand = compile_expr(c, print->operand);
        qbe_call_add_arg(c->qbe, call, qbe_build_cast(c->qbe, c->fn, operand, QBE_TYPE_I64, true));
    } break;

    default:
        todo();
        break;
    }
}

void compile_nodes(Nodes ns, const char *output) {
    Compiler c = {0};
    c.qbe = qbe_new();

    c.fn = qbe_fn_new(c.qbe, qbe_sv_from_cstr("main"), qbe_type_basic(QBE_TYPE_I32));
    for (Node *it = ns.head; it; it = it->next) {
        compile_stmt(&c, it);
    }
    qbe_build_return(c.qbe, c.fn, qbe_atom_int(c.qbe, QBE_TYPE_I32, 0));

    const int code = qbe_generate(c.qbe, QBE_TARGET_DEFAULT, output, NULL, 0);
    if (code) {
        exit(code);
    }
}
