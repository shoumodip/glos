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
    POWER_LOR,
    POWER_CMP,
    POWER_SHL,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_PRE,
    POWER_DOT
} Power;

static_assert(COUNT_TOKENS == 38, "");
static Power tokenKindPower(TokenKind kind) {
    switch (kind) {
    case TOKEN_LPAREN:
        return POWER_DOT;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
        return POWER_MUL;

    case TOKEN_SHL:
    case TOKEN_SHR:
        return POWER_SHL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_SET:
        return POWER_SET;

    case TOKEN_LOR:
    case TOKEN_LAND:
        return POWER_LOR;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return POWER_CMP;

    default:
        return POWER_NIL;
    }
}

static void errorUnexpected(Token token) {
    fprintf(stderr, PosFmt "ERROR: Unexpected %s\n", PosArg(token.pos), tokenKindName(token.kind));
    exit(1);
}

static bool tokenKindIsStartOfType(TokenKind k) {
    switch (k) {
    case TOKEN_IDENT:
    case TOKEN_BAND:
    case TOKEN_LAND:
    case TOKEN_FN:
        return true;

    default:
        return false;
    }
}

static Node *parseType(Parser *p);
static Node *parseExpr(Parser *p, Power mbp);

static Node *parseArgWithOptionalName(Parser *p, Node *fn) {
    const Token argToken = lexerNext(&p->lexer);

    Node *arg = nodeNew(p, NODE_ARG, argToken);
    if (argToken.kind == TOKEN_IDENT) {
        const Token peek = lexerPeek(&p->lexer);
        if (peek.kind == TOKEN_COMMA || peek.kind == TOKEN_RPAREN) {
            arg->token = fn->token;
            arg->as.arg.type = nodeNew(p, NODE_ATOM, argToken);
        } else {
            arg->as.arg.type = parseType(p);
        }
    } else {
        lexerBuffer(&p->lexer, argToken);
        arg->token = fn->token;
        arg->as.arg.type = parseType(p);
    }

    arg->as.arg.index = fn->as.fn.arity++;
    return arg;
}

static_assert(COUNT_TOKENS == 38, "");
static Node *parseType(Parser *p) {
    Node *node = NULL;
    Token token = lexerNext(&p->lexer);

    switch (token.kind) {
    case TOKEN_IDENT:
        node = nodeNew(p, NODE_ATOM, token);
        break;

    case TOKEN_BAND:
        node = nodeNew(p, NODE_UNARY, token);
        node->as.unary.operand = parseType(p);
        break;

    case TOKEN_LAND:
        node = nodeNew(p, NODE_UNARY, lexerSplitToken(&p->lexer, token));
        node->as.unary.operand = parseType(p);
        break;

    case TOKEN_FN:
        node = nodeNew(p, NODE_FN, token);
        lexerExpect(&p->lexer, TOKEN_LPAREN);

        while (!lexerRead(&p->lexer, TOKEN_RPAREN)) {
            nodesPush(&node->as.fn.args, parseArgWithOptionalName(p, node));
            if (lexerExpect(&p->lexer, TOKEN_RPAREN, TOKEN_COMMA).kind == TOKEN_RPAREN) {
                break;
            }
        }

        token = lexerPeek(&p->lexer);
        if (!token.onNewline && tokenKindIsStartOfType(token.kind)) {
            node->as.fn.ret = parseType(p);
        }
        break;

    default:
        errorUnexpected(token);
    }

    return node;
}

static Node *parseFn(Parser *p, Token name);

static_assert(COUNT_TOKENS == 38, "");
static Node *parseExpr(Parser *p, Power mbp) {
    Node *node = NULL;
    Token token = lexerNext(&p->lexer);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_IDENT:
        node = nodeNew(p, NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BNOT:
    case TOKEN_LNOT:
    case TOKEN_BAND:
        node = nodeNew(p, NODE_UNARY, token);
        node->as.unary.operand = parseExpr(p, POWER_PRE);
        break;

    case TOKEN_LPAREN:
        node = parseExpr(p, POWER_SET);
        lexerExpect(&p->lexer, TOKEN_RPAREN);
        break;

    case TOKEN_LT: {
        node = nodeNew(p, NODE_CAST, token);
        node->as.cast.to = parseType(p);

        lexerExpect(&p->lexer, TOKEN_GT);
        node->as.cast.from = parseExpr(p, POWER_PRE);
    } break;

    case TOKEN_SIZEOF:
        node = nodeNew(p, NODE_SIZEOF, token);

        token = lexerExpect(&p->lexer, TOKEN_LPAREN, TOKEN_LT);
        if (token.kind == TOKEN_LPAREN) {
            node->as.sizeoff.isExpr = true;
            node->as.sizeoff.operand = parseExpr(p, POWER_SET);
            lexerExpect(&p->lexer, TOKEN_RPAREN);
        } else {
            node->as.sizeoff.operand = parseType(p);
            lexerExpect(&p->lexer, TOKEN_GT);
        }
        break;

    case TOKEN_FN:
        node = parseFn(p, token);
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

        switch (token.kind) {
        case TOKEN_LPAREN: {
            Node *call = nodeNew(p, NODE_CALL, token);
            call->as.call.fn = node;

            while (!lexerRead(&p->lexer, TOKEN_RPAREN)) {
                nodesPush(&call->as.call.args, parseExpr(p, POWER_SET));
                call->as.call.arity++;

                if (lexerExpect(&p->lexer, TOKEN_RPAREN, TOKEN_COMMA).kind == TOKEN_RPAREN) {
                    break;
                }
            }

            node = call;
        } break;

        default: {
            Node *binary = nodeNew(p, NODE_BINARY, token);
            binary->as.binary.lhs = node;
            binary->as.binary.rhs = parseExpr(p, lbp);
            node = binary;
        } break;
        }
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

static void consumeEols(Parser *p) {
    while (lexerRead(&p->lexer, TOKEN_EOL));
}

static_assert(COUNT_TOKENS == 38, "");
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

    case TOKEN_FOR:
        localAssert(p, token, true);
        node = nodeNew(p, NODE_FOR, token);

        token = lexerPeek(&p->lexer);
        if (token.kind == TOKEN_VAR) {
            p->dontConsumeEols = true;
            node->as.forr.condition = parseStmt(p);
            p->dontConsumeEols = false;

            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_EOL));
        } else {
            node->as.forr.condition = parseExpr(p, POWER_NIL);

            Node *cond = node->as.forr.condition;
            if (cond->kind == NODE_BINARY && tokenKindPower(cond->token.kind) == POWER_SET) {
                lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_EOL));
            }
        }

        if (lexerRead(&p->lexer, TOKEN_EOL)) {
            consumeEols(p);
            node->as.forr.init = node->as.forr.condition;
            node->as.forr.condition = parseExpr(p, POWER_SET);

            if (lexerRead(&p->lexer, TOKEN_EOL)) {
                consumeEols(p);
                node->as.forr.update = parseExpr(p, POWER_NIL);
            }
        }

        lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE));
        node->as.forr.body = parseStmt(p);
        break;

    case TOKEN_RETURN:
        localAssert(p, token, true);
        node = nodeNew(p, NODE_FLOW, token);

        token = lexerPeek(&p->lexer);
        if (!token.onNewline) {
            node->as.flow.operand = parseExpr(p, POWER_SET);
        }
        break;

    case TOKEN_FN:
        node = parseFn(p, lexerExpect(&p->lexer, TOKEN_IDENT));
        break;

    case TOKEN_VAR:
        node = nodeNew(p, NODE_VAR, lexerExpect(&p->lexer, TOKEN_IDENT));

        if (p->inExtern) {
            node->as.var.isExtern = true;
            node->as.var.type = parseType(p);
        } else {
            token = lexerPeek(&p->lexer);
            if (token.kind != TOKEN_SET) {
                node->as.var.type = parseType(p);
            }

            if (lexerRead(&p->lexer, TOKEN_SET)) {
                node->as.var.expr = parseExpr(p, POWER_SET);
            }
        }

        node->as.var.local = p->local;
        break;

    case TOKEN_EXTERN: {
        assert(!p->inExtern);
        node = nodeNew(p, NODE_EXTERN, token);
        lexerExpect(&p->lexer, TOKEN_LBRACE);

        p->inExtern = true;
        while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_FN, TOKEN_VAR));
            nodesPush(&node->as.externn.definitions, parseStmt(p));
        }
        p->inExtern = false;
    } break;

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

    if (!p->dontConsumeEols) {
        consumeEols(p);
    }
    return node;
}

static Node *parseFn(Parser *p, Token name) {
    Node *node = nodeNew(p, NODE_FN, name);

    const bool localSave = p->local;
    p->local = true;

    {
        lexerExpect(&p->lexer, TOKEN_LPAREN);
        while (!lexerRead(&p->lexer, TOKEN_RPAREN)) {
            Node *arg = NULL;
            if (p->inExtern) {
                arg = parseArgWithOptionalName(p, node);
            } else {
                arg = nodeNew(p, NODE_ARG, lexerExpect(&p->lexer, TOKEN_IDENT));
                arg->as.arg.type = parseType(p);
                arg->as.arg.index = node->as.fn.arity++;
            }
            nodesPush(&node->as.fn.args, arg);

            if (lexerExpect(&p->lexer, TOKEN_RPAREN, TOKEN_COMMA).kind == TOKEN_RPAREN) {
                break;
            }
        }

        const Token peek = lexerPeek(&p->lexer);
        if (!peek.onNewline && tokenKindIsStartOfType(peek.kind)) {
            node->as.fn.ret = parseType(p);
        }

        if (!p->inExtern) {
            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE));
            node->as.fn.body = parseStmt(p);
        }
    }

    p->local = localSave;
    return node;
}

void parseFile(Parser *p, Lexer lexer) {
    p->lexer = lexer;
    while (!lexerRead(&p->lexer, TOKEN_EOF)) {
        consumeEols(p);
        if (lexerRead(&p->lexer, TOKEN_EOF)) {
            break;
        }

        nodesPush(&p->nodes, parseStmt(p));
    }
}
