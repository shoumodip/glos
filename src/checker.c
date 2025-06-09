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

static void error_undefined(const Node *n, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Undefined %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    exit(1);
}

static_assert(COUNT_NODES == 8, "");
static void check_type(Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM:
        if (sv_match(n->token.sv, "bool")) {
            n->type = (Type) {.kind = TYPE_BOOL};
        } else if (sv_match(n->token.sv, "i64")) {
            n->type = (Type) {.kind = TYPE_I64};
        } else {
            error_undefined(n, "type");
        }
        break;

    case NODE_FN:
        n->type = (Type) {.kind = TYPE_FN};
        break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 8, "");
static void check_expr(Context *c, Node *n, bool ref) {
    if (!n) {
        return;
    }

    bool allow_ref = false;
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 19, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_I64};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_IDENT:
            atom->definition = scope_find(c->globals, n->token.sv);
            if (!atom->definition) {
                error_undefined(n, "identifier");
            }

            allow_ref = atom->definition->kind == NODE_VAR;
            n->type = atom->definition->type;
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 19, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->operand, false);
            n->type = type_assert_arith(unary->operand);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 19, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs);
            n->type = type_assert_node(binary->rhs, binary->lhs);
            break;

        case TOKEN_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert_node(binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }

    if (!allow_ref && ref) {
        fprintf(stderr, PosFmt "ERROR: Cannot take reference to value not in memory\n", PosArg(n->token.pos));
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Node *previous, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Redefinition of %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    fprintf(stderr, PosFmt "NOTE: Defined here\n", PosArg(previous->token.pos));
    exit(1);
}

static_assert(COUNT_NODES == 8, "");
static void check_stmt(Context *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        check_expr(c, iff->condition, false);
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
        n->type = (Type) {.kind = TYPE_FN};

        da_push(&c->globals, n);
        check_stmt(c, fn->body);
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (!var->local) {
            const Node *previous = scope_find(c->globals, n->token.sv);
            if (previous) {
                error_redefinition(n, previous, "identifier");
            }
        }

        if (var->type) {
            check_type(var->type);
            n->type = var->type->type;
        }

        if (var->expr) {
            check_expr(c, var->expr, false);
            n->type = var->expr->type;

            if (n->type.kind == TYPE_UNIT) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Cannot define variable with type '%s'\n",
                    PosArg(n->token.pos),
                    type_to_cstr(n->type));

                exit(1);
            }

            if (var->type) {
                type_assert(var->expr, var->type->type);
                n->type = var->expr->type;
            }
        }

        if (var->local) {
            todo();
        } else {
            da_push(&c->globals, n);
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        check_expr(c, print->operand, false);
        type_assert_scalar(print->operand);
    } break;

    default:
        check_expr(c, n, false);
        break;
    }
}

void check_nodes(Context *c, Nodes ns) {
    for (Node *it = ns.head; it; it = it->next) {
        check_stmt(c, it);
    }
}
