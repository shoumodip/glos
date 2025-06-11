#include "parser.h"

static void nodes_push(Nodes *ns, Node *n) {
    if (ns->tail) {
        ns->tail->next = n;
        ns->tail = n;
    } else {
        ns->head = n;
        ns->tail = n;
    }
}

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_ADD,
    POWER_MUL,
    POWER_PRE,
    POWER_DOT
} Power;

static_assert(COUNT_TOKENS == 19, "");
static Power token_kind_to_power(TokenKind kind) {
    switch (kind) {
    case TOKEN_LPAREN:
    case TOKEN_RPAREN:
        return POWER_DOT;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
        return POWER_MUL;

    case TOKEN_SET:
        return POWER_SET;

    default:
        return POWER_NIL;
    }
}

static_assert(COUNT_NODES == 9, "");
static void *node_alloc(Parser *p, NodeKind kind, Token token) {
    static const size_t sizes[COUNT_NODES] = {
        [NODE_ATOM] = sizeof(NodeAtom),
        [NODE_CALL] = sizeof(NodeCall),
        [NODE_UNARY] = sizeof(NodeUnary),
        [NODE_BINARY] = sizeof(NodeBinary),

        [NODE_IF] = sizeof(NodeIf),
        [NODE_BLOCK] = sizeof(NodeBlock),

        [NODE_FN] = sizeof(NodeFn),
        [NODE_VAR] = sizeof(NodeVar),

        [NODE_PRINT] = sizeof(NodePrint),
    };

    assert(kind >= NODE_ATOM && kind < COUNT_NODES);
    const size_t size = sizes[kind];

    Node *node = arena_alloc(p->arena, size);
    node->kind = kind;
    node->token = token;
    return node;
}

static void error_unexpected(Token token) {
    fprintf(stderr, PosFmt "ERROR: Unexpected %s\n", PosArg(token.pos), token_kind_to_cstr(token.kind));
    exit(1);
}

static_assert(COUNT_TOKENS == 19, "");
static Node *parse_type(Parser *p) {
    Node *node = NULL;
    Token token = lexer_next(&p->lexer);

    switch (token.kind) {
    case TOKEN_IDENT:
        node = node_alloc(p, NODE_ATOM, token);
        break;

    case TOKEN_FN:
        node = node_alloc(p, NODE_FN, token);
        lexer_expect(&p->lexer, TOKEN_LPAREN);
        lexer_expect(&p->lexer, TOKEN_RPAREN);
        break;

    default:
        error_unexpected(token);
        break;
    }

    return node;
}

static_assert(COUNT_TOKENS == 19, "");
static Node *parse_expr(Parser *p, Power mbp) {
    Node *node = NULL;
    Token token = lexer_next(&p->lexer);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_IDENT:
        node = node_alloc(p, NODE_ATOM, token);
        break;

    case TOKEN_SUB: {
        NodeUnary *unary = node_alloc(p, NODE_UNARY, token);
        unary->operand = parse_expr(p, POWER_PRE);
        node = (Node *) unary;
    } break;

    case TOKEN_LPAREN:
        node = parse_expr(p, POWER_SET);
        lexer_expect(&p->lexer, TOKEN_RPAREN);
        break;

    default:
        error_unexpected(token);
    }

    while (true) {
        token = lexer_peek(&p->lexer);
        if (token.newline) {
            break;
        }

        const Power lbp = token_kind_to_power(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexer_unbuffer(&p->lexer);

        switch (token.kind) {
        case TOKEN_LPAREN: {
            NodeCall *call = node_alloc(p, NODE_CALL, token);
            call->fn = node;
            lexer_expect(&p->lexer, TOKEN_RPAREN);
            node = (Node *) call;
        } break;

        default: {
            NodeBinary *binary = node_alloc(p, NODE_BINARY, token);
            binary->lhs = node;
            binary->rhs = parse_expr(p, lbp);
            node = (Node *) binary;
        } break;
        }
    }

    return node;
}

static void consume_eols(Parser *p) {
    while (lexer_read(&p->lexer, TOKEN_EOL));
}

static void local_assert(Parser *p, Token token, bool local) {
    if (p->local != local) {
        fprintf(
            stderr,
            PosFmt "ERROR: Unexpected %s in %s scope\n",
            PosArg(token.pos),
            token_kind_to_cstr(token.kind),
            p->local ? "local" : "global");

        exit(1);
    }
}

static_assert(COUNT_TOKENS == 19, "");
static Node *parse_stmt(Parser *p) {
    Node *node = NULL;

    Token token = lexer_next(&p->lexer);
    switch (token.kind) {
    case TOKEN_LBRACE: {
        local_assert(p, token, true);
        NodeBlock *block = node_alloc(p, NODE_BLOCK, token);
        while (!lexer_read(&p->lexer, TOKEN_RBRACE)) {
            nodes_push(&block->body, parse_stmt(p));
        }

        assert(p->lexer.buffer.kind == TOKEN_RBRACE);
        block->node.token = p->lexer.buffer;

        node = (Node *) block;
    } break;

    case TOKEN_IF: {
        local_assert(p, token, true);

        NodeIf *iff = node_alloc(p, NODE_IF, token);
        iff->condition = parse_expr(p, POWER_SET);

        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        iff->consequence = parse_stmt(p);

        if (lexer_read(&p->lexer, TOKEN_ELSE)) {
            lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE, TOKEN_IF));
            iff->antecedence = parse_stmt(p);
        }

        node = (Node *) iff;
    } break;

    case TOKEN_FN: {
        local_assert(p, token, false);

        const bool local_save = p->local;
        p->local = true;

        NodeFn *fn = node_alloc(p, NODE_FN, lexer_expect(&p->lexer, TOKEN_IDENT));
        lexer_expect(&p->lexer, TOKEN_LPAREN);
        lexer_expect(&p->lexer, TOKEN_RPAREN);

        lexer_buffer(&p->lexer, lexer_expect(&p->lexer, TOKEN_LBRACE));
        fn->body = parse_stmt(p);

        p->local = local_save;

        node = (Node *) fn;
    } break;

    case TOKEN_VAR: {
        NodeVar *var = node_alloc(p, NODE_VAR, lexer_expect(&p->lexer, TOKEN_IDENT));
        token = lexer_peek(&p->lexer);
        if (token.kind != TOKEN_SET) {
            var->type = parse_type(p);
        }

        if (lexer_read(&p->lexer, TOKEN_SET)) {
            var->expr = parse_expr(p, POWER_SET);
        }

        var->local = p->local;
        node = (Node *) var;
    } break;

    case TOKEN_PRINT: {
        local_assert(p, token, true);
        NodePrint *print = node_alloc(p, NODE_PRINT, token);
        print->operand = parse_expr(p, POWER_SET);
        node = (Node *) print;
    } break;

    default:
        local_assert(p, token, true);
        lexer_buffer(&p->lexer, token);
        node = parse_expr(p, POWER_NIL);
        break;
    }

    consume_eols(p);
    return node;
}

void parse_file(Parser *p, Lexer lexer) {
    assert(p->arena);

    p->lexer = lexer;
    while (true) {
        consume_eols(p);
        if (lexer_read(&p->lexer, TOKEN_EOF)) {
            break;
        }

        nodes_push(&p->nodes, parse_stmt(p));
    }
}
