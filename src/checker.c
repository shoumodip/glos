#include "checker.h"

static Type typeAssert(const Node *n, Type expected) {
    if (!typeEq(n->type, expected)) {
        fprintf(
            stderr,
            PosFmt "ERROR: Expected type '%s', got '%s'\n",
            PosArg(n->token.pos),
            typeToString(expected),
            typeToString(n->type));

        exit(1);
    }
    return n->type;
}

static Type typeAssertArith(const Node *n) {
    if (n->type.kind != TYPE_I64) {
        fprintf(
            stderr,
            PosFmt "ERROR: Expected arithmetic type, got '%s'\n",
            PosArg(n->token.pos),
            typeToString(n->type));

        exit(1);
    }
    return n->type;
}

static Type typeAssertScalar(const Node *n) {
    if (n->type.kind != TYPE_I64 && n->type.kind != TYPE_BOOL) {
        fprintf(
            stderr,
            PosFmt "ERROR: Expected scalar type, got '%s'\n",
            PosArg(n->token.pos),
            typeToString(n->type));

        exit(1);
    }
    return n->type;
}

static_assert(COUNT_NODES == 4, "");
static void checkNode(Node *n) {
    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 11, "");
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
        break;

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        static_assert(COUNT_TOKENS == 11, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            checkNode(operand);
            n->type = typeAssertArith(operand);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 11, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            checkNode(lhs);
            checkNode(rhs);
            n->type = typeAssert(rhs, typeAssertArith(lhs));
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_PRINT:
        checkNode(n->as.print.operand);
        typeAssertScalar(n->as.print.operand);
        break;

    default:
        unreachable();
    }
}

void checkNodes(Nodes nodes) {
    for (Node *it = nodes.head; it; it = it->next) {
        checkNode(it);
    }
}
