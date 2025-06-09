#include "checker.h"

static Type type_assert(Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return n->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(n->token.pos),
        type_to_cstr(expected),
        type_to_cstr(n->type));

    exit(1);
}

static Type type_assert_node(Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(a->token.pos),
        type_to_cstr(b->type),
        type_to_cstr(a->type));

    exit(1);
}

static Type type_assert_arith(const Node *n) {
    if (type_is_integer(n->type)) {
        return n->type;
    }

    fprintf(stderr, PosFmt "ERROR: Expected arithmetic type, got '%s'\n", PosArg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_scalar(const Node *n) {
    if (type_is_integer(n->type)) {
        return n->type;
    }

    if (type_eq(n->type, (Type) {.kind = TYPE_BOOL})) {
        return n->type;
    }

    fprintf(stderr, PosFmt "ERROR: Expected scalar type, got '%s'\n", PosArg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static_assert(COUNT_NODES == 7, "");
static void check_expr(Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 17, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_I64};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 17, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(unary->operand);
            n->type = type_assert_arith(unary->operand);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 17, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            check_expr(binary->lhs);
            check_expr(binary->rhs);
            type_assert_arith(binary->lhs);
            n->type = type_assert_node(binary->rhs, binary->lhs);
            break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static void error_redefinition(const Node *n, const Node *previous, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Redefinition of %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    fprintf(stderr, PosFmt "NOTE: Defined here\n", PosArg(previous->token.pos));
    exit(1);
}

static_assert(COUNT_NODES == 7, "");
static void check_stmt(Context *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        check_expr(iff->condition);
        type_assert(iff->condition, (Type) {.kind = TYPE_BOOL});

        check_stmt(c, iff->consequence);
        check_stmt(c, iff->antecedence);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }
    } break;

    case NODE_FN: {
        const Node *previous = scope_find(c->globals, n->token.sv);
        if (previous) {
            error_redefinition(n, previous, "identifier");
        }

        NodeFn *fn = (NodeFn *) n;
        da_push(&c->globals, n);
        check_stmt(c, fn->body);
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        check_expr(print->operand);
        type_assert_scalar(print->operand);
    } break;

    default:
        check_expr(n);
        break;
    }
}

void check_nodes(Context *c, Nodes ns) {
    for (Node *it = ns.head; it; it = it->next) {
        check_stmt(c, it);
    }
}
