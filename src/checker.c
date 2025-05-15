#include "checker.h"

// Scope
static void scopePush(Scope *s, Node *n) {
    if (s->length >= s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 128;
        s->data = realloc(s->data, s->capacity * sizeof(*s->data));
        assert(s->data);
    }

    s->data[s->length++] = n;
}

Node *scopeFind(Scope s, Str name) {
    for (size_t i = s.length; i > 0; i--) {
        Node *it = s.data[i - 1];
        if (strEq(it->token.str, name)) {
            return it;
        }
    }

    return NULL;
}

// Checker
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

static void errorRedefinition(const Node *n, const Node *previous, const char *label) {
    fprintf(
        stderr,
        PosFmt "ERROR: Redefinition of %s '" StrFmt "'\n",
        PosArg(n->token.pos),
        label,
        StrArg(n->token.str));

    fprintf(stderr, PosFmt "NOTE: Defined here\n", PosArg(previous->token.pos));
    exit(1);
}

static_assert(COUNT_NODES == 6, "");
static void checkNode(Context *c, Node *n) {
    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 14, "");
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

        static_assert(COUNT_TOKENS == 14, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            checkNode(c, operand);
            n->type = typeAssertArith(operand);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 14, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            checkNode(c, lhs);
            checkNode(c, rhs);
            n->type = typeAssert(rhs, typeAssertArith(lhs));
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BLOCK:
        for (Node *it = n->as.block.head; it; it = it->next) {
            checkNode(c, it);
        }
        break;

    case NODE_FN:
        assert(!n->as.fn.args);
        assert(!n->as.fn.ret);

        {
            const Node *previous = scopeFind(c->globals, n->token.str);
            if (previous) {
                errorRedefinition(n, previous, "identifier");
            }
        }

        scopePush(&c->globals, n);
        checkNode(c, n->as.fn.body);

        n->type = (Type) {.kind = TYPE_FN};
        break;

    case NODE_PRINT:
        checkNode(c, n->as.print.operand);
        typeAssertScalar(n->as.print.operand);
        break;

    default:
        unreachable();
    }
}

void checkNodes(Context *c, Nodes nodes) {
    for (Node *it = nodes.head; it; it = it->next) {
        checkNode(c, it);
    }
}
