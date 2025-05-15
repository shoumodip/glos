#include "parser.h"

static Node *nodeNew(Parser *p, NodeKind kind, Token token) {
#define NODE_POOL_CAP 16000

    if (!p->pool.data) {
        p->pool.data = malloc(NODE_POOL_CAP * sizeof(*p->pool.data));
        assert(p->pool.data);
    }
    assert(p->pool.length < NODE_POOL_CAP);

#undef NODE_POOL_CAP

    Node *node = &p->pool.data[p->pool.length++];
    *node = (Node) {
        .kind = kind,
        .token = token,
    };
    return node;
}

static void nodesPush(Nodes *ns, Node *node) {
    if (ns->tail) {
        ns->tail->next = node;
        ns->tail = node;
    } else {
        ns->head = node;
        ns->tail = node;
    }
}

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_ADD,
    POWER_MUL,
    POWER_PRE
} Power;

static_assert(COUNT_TOKENS == 16, "");
static Power tokenKindPower(TokenKind kind) {
    switch (kind) {
    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
        return POWER_MUL;

    default:
        return POWER_NIL;
    }
}

static void errorUnexpected(Token token) {
    fprintf(stderr, PosFmt "ERROR: Unexpected %s\n", PosArg(token.pos), tokenKindName(token.kind));
    exit(1);
}

static_assert(COUNT_TOKENS == 16, "");
static Node *parseExpr(Parser *p, Power mbp) {
    Node *node = NULL;
    Token token = lexerNext(&p->lexer);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
        node = nodeNew(p, NODE_ATOM, token);
        break;

    case TOKEN_SUB:
        node = nodeNew(p, NODE_UNARY, token);
        node->as.unary.operand = parseExpr(p, POWER_PRE);
        break;

    case TOKEN_LPAREN:
        node = parseExpr(p, POWER_SET);
        lexerExpect(&p->lexer, TOKEN_RPAREN);
        break;

    default:
        errorUnexpected(token);
    }

    while (true) {
        token = lexerPeek(&p->lexer);
        if (token.onNewline) {
            break;
        }

        const Power lbp = tokenKindPower(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexerUnbuffer(&p->lexer);

        Node *binary = nodeNew(p, NODE_BINARY, token);
        binary->as.binary.lhs = node;
        binary->as.binary.rhs = parseExpr(p, lbp);
        node = binary;
    }

    return node;
}

static void localAssert(Parser *p, Token token, bool local) {
    if (p->local != local) {
        fprintf(
            stderr,
            PosFmt "ERROR: Unexpected %s in %s scope\n",
            PosArg(token.pos),
            tokenKindName(token.kind),
            p->local ? "local" : "global");

        exit(1);
    }
}

static_assert(COUNT_TOKENS == 16, "");
static Node *parseStmt(Parser *p) {
    Node *node = NULL;

    Token token = lexerNext(&p->lexer);
    switch (token.kind) {
    case TOKEN_LBRACE:
        localAssert(p, token, true);
        node = nodeNew(p, NODE_BLOCK, token);
        while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
            nodesPush(&node->as.block, parseStmt(p));
        }

        assert(p->lexer.buffer.kind == TOKEN_RBRACE);
        node->token = p->lexer.buffer;
        break;

    case TOKEN_IF:
        localAssert(p, token, true);
        node = nodeNew(p, NODE_IF, token);
        node->as.iff.condition = parseExpr(p, POWER_SET);

        lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE));
        node->as.iff.consequence = parseStmt(p);

        if (lexerRead(&p->lexer, TOKEN_ELSE)) {
            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE, TOKEN_IF));
            node->as.iff.antecedence = parseStmt(p);
        }
        break;

    case TOKEN_FN:
        localAssert(p, token, false);
        node = nodeNew(p, NODE_FN, lexerExpect(&p->lexer, TOKEN_IDENT));

        const bool localSave = p->local;
        p->local = true;

        {
            lexerExpect(&p->lexer, TOKEN_LPAREN);
            lexerExpect(&p->lexer, TOKEN_RPAREN);

            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE));
            node->as.fn.body = parseStmt(p);
        }

        p->local = localSave;
        break;

    case TOKEN_PRINT:
        localAssert(p, token, true);
        node = nodeNew(p, NODE_PRINT, token);
        node->as.print.operand = parseExpr(p, POWER_SET);
        break;

    default:
        localAssert(p, token, true);
        lexerBuffer(&p->lexer, token);
        node = parseExpr(p, POWER_NIL);
        break;
    }

    return node;
}

void parseFile(Parser *p, Lexer lexer) {
    p->lexer = lexer;
    while (!lexerRead(&p->lexer, TOKEN_EOF)) {
        nodesPush(&p->nodes, parseStmt(p));
    }
}
