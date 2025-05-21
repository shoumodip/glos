#include "parser.h"

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

static_assert(COUNT_TOKENS == 49, "");
static Power tokenKindPower(TokenKind kind) {
    switch (kind) {
    case TOKEN_DOT:
    case TOKEN_LPAREN:
    case TOKEN_LBRACE:
    case TOKEN_LBRACKET:
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
    case TOKEN_WALRUS:
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
    case TOKEN_LBRACKET:
    case TOKEN_BAND:
    case TOKEN_LAND:
    case TOKEN_FN:
    case TOKEN_STRUCT:
        return true;

    default:
        return false;
    }
}

static Node *parseType(Parser *p);

static Node *parseArgWithOptionalName(Parser *p, Node *fn) {
    const Token argToken = lexerNext(&p->lexer);

    Node *arg = nodeAlloc(p->nodeAlloc, NODE_ARG, argToken);
    if (argToken.kind == TOKEN_IDENT) {
        const Token peek = lexerPeek(&p->lexer);
        if (peek.kind == TOKEN_COMMA || peek.kind == TOKEN_RPAREN) {
            arg->token = fn->token;
            arg->as.arg.type = nodeAlloc(p->nodeAlloc, NODE_ATOM, argToken);
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

// TODO: Proper constant parsing
static Node *parseConst(Parser *p) {
    return nodeAlloc(p->nodeAlloc, NODE_ATOM, lexerExpect(&p->lexer, TOKEN_INT));
}

static_assert(COUNT_TOKENS == 49, "");
static Node *parseType(Parser *p) {
    Node *node = NULL;
    Token token = lexerNext(&p->lexer);

    switch (token.kind) {
    case TOKEN_IDENT:
        node = nodeAlloc(p->nodeAlloc, NODE_ATOM, token);
        break;

    case TOKEN_LBRACKET: {
        node = nodeAlloc(p->nodeAlloc, NODE_ARRAY, token);
        node->as.array.base = parseType(p);

        token = lexerExpect(&p->lexer, TOKEN_EOL, TOKEN_RBRACKET);
        if (token.kind == TOKEN_EOL) {
            node->as.array.length = parseConst(p);
            lexerExpect(&p->lexer, TOKEN_RBRACKET);
        }
    } break;

    case TOKEN_BAND:
        node = nodeAlloc(p->nodeAlloc, NODE_UNARY, token);
        node->as.unary.operand = parseType(p);
        break;

    case TOKEN_LAND:
        node = nodeAlloc(p->nodeAlloc, NODE_UNARY, lexerSplitToken(&p->lexer, token));
        node->as.unary.operand = parseType(p);
        break;

    case TOKEN_FN:
        node = nodeAlloc(p->nodeAlloc, NODE_FN, token);
        lexerExpect(&p->lexer, TOKEN_LPAREN);

        while (!lexerRead(&p->lexer, TOKEN_RPAREN)) {
            nodesPush(&node->as.fn.args, parseArgWithOptionalName(p, node));
            if (lexerExpect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN).kind == TOKEN_RPAREN) {
                break;
            }
        }

        token = lexerPeek(&p->lexer);
        if (!token.onNewline && tokenKindIsStartOfType(token.kind)) {
            node->as.fn.ret = parseType(p);
        }
        break;

    case TOKEN_STRUCT:
        node = nodeAlloc(p->nodeAlloc, NODE_STRUCT, token);
        lexerExpect(&p->lexer, TOKEN_LBRACE);

        while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
            Node *field = nodeAlloc(p->nodeAlloc, NODE_FIELD, lexerExpect(&p->lexer, TOKEN_IDENT));
            field->as.field.type = parseType(p);
            field->as.field.index = node->as.structt.fieldsCount++;
            nodesPush(&node->as.structt.fields, field);

            lexerRead(&p->lexer, TOKEN_COMMA);
        }
        break;

    default:
        errorUnexpected(token);
    }

    return node;
}

static Node *parseFn(Parser *p, Token name);

static_assert(COUNT_TOKENS == 49, "");
static Node *parseExpr(Parser *p, Power mbp, bool noStruct) {
    Node *node = NULL;
    Token token = lexerNext(&p->lexer);

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_STR:
    case TOKEN_CSTR:
    case TOKEN_CHAR:
    case TOKEN_IDENT:
        node = nodeAlloc(p->nodeAlloc, NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BNOT:
    case TOKEN_LNOT:
    case TOKEN_BAND:
        node = nodeAlloc(p->nodeAlloc, NODE_UNARY, token);
        node->as.unary.operand = parseExpr(p, POWER_PRE, noStruct);
        break;

    case TOKEN_LBRACKET:
        node = nodeAlloc(p->nodeAlloc, NODE_ARRAY, token);
        node->as.array.base = parseType(p);

        lexerExpect(&p->lexer, TOKEN_EOL);
        node->as.array.length = parseConst(p);
        lexerExpect(&p->lexer, TOKEN_RBRACKET);

        lexerExpect(&p->lexer, TOKEN_LBRACE);
        while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
            Node *index = parseExpr(p, POWER_SET, false);
            token = lexerExpect(&p->lexer, TOKEN_COLON); // TODO: non-designated initialization

            Node *assign = nodeAlloc(p->nodeAlloc, NODE_BINARY, token);
            assign->as.binary.lhs = index;
            assign->as.binary.rhs = parseExpr(p, POWER_SET, false);

            nodesPush(&node->as.array.literalInits, assign);
            if (lexerExpect(&p->lexer, TOKEN_COMMA, TOKEN_RBRACE).kind == TOKEN_RBRACE) {
                break;
            }
        }
        break;

    case TOKEN_LPAREN:
        node = parseExpr(p, POWER_SET, false);
        lexerExpect(&p->lexer, TOKEN_RPAREN);
        break;

    case TOKEN_LT: {
        node = nodeAlloc(p->nodeAlloc, NODE_CAST, token);
        node->as.cast.to = parseType(p);

        lexerExpect(&p->lexer, TOKEN_GT);
        node->as.cast.from = parseExpr(p, POWER_PRE, noStruct);
    } break;

    case TOKEN_SIZEOF:
        node = nodeAlloc(p->nodeAlloc, NODE_SIZEOF, token);

        token = lexerExpect(&p->lexer, TOKEN_LPAREN, TOKEN_LT);
        if (token.kind == TOKEN_LPAREN) {
            node->as.sizeoff.isExpr = true;
            node->as.sizeoff.operand = parseExpr(p, POWER_SET, false);
            lexerExpect(&p->lexer, TOKEN_RPAREN);
        } else {
            node->as.sizeoff.operand = parseType(p);
            lexerExpect(&p->lexer, TOKEN_GT);
        }
        break;

    case TOKEN_FN:
        node = parseFn(p, token);
        break;

    case TOKEN_STRUCT:
        lexerBuffer(&p->lexer, token);
        node = parseType(p);
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
        case TOKEN_DOT: {
            Node *dot = nodeAlloc(p->nodeAlloc, NODE_MEMBER, token);
            dot->as.member.lhs = node;

            token = lexerExpect(&p->lexer, TOKEN_IDENT);
            dot->as.member.rhs = nodeAlloc(p->nodeAlloc, NODE_ATOM, token);

            node = dot;
        } break;

        case TOKEN_WALRUS:
            if (node->kind != NODE_ATOM || node->token.kind != TOKEN_IDENT) {
                errorUnexpected(token);
            }

            node->kind = NODE_VAR;
            node->as.var.expr = parseExpr(p, POWER_SET, false);
            node->as.var.local = true;
            return node;

        case TOKEN_LPAREN: {
            Node *call = nodeAlloc(p->nodeAlloc, NODE_CALL, token);
            call->as.call.fn = node;

            while (!lexerRead(&p->lexer, TOKEN_RPAREN)) {
                nodesPush(&call->as.call.args, parseExpr(p, POWER_SET, false));
                call->as.call.arity++;

                if (lexerExpect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN).kind == TOKEN_RPAREN) {
                    break;
                }
            }

            node = call;
        } break;

        case TOKEN_LBRACE: {
            bool ok = false;
            if (!noStruct) {
                if (node->kind == NODE_ATOM && node->token.kind == TOKEN_IDENT) {
                    ok = true;
                } else if (node->kind == NODE_STRUCT && !node->as.structt.literalType) {
                    ok = true;
                }
            }

            if (!ok) {
                lexerBuffer(&p->lexer, token);
                return node;
            }

            Node *structt = nodeAlloc(p->nodeAlloc, NODE_STRUCT, token);
            structt->as.structt.literalType = node;

            while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
                token = lexerExpect(&p->lexer, TOKEN_IDENT);
                Node *assign = nodeAlloc(p->nodeAlloc, NODE_BINARY, token);

                lexerExpect(&p->lexer, TOKEN_COLON);
                assign->as.binary.rhs = parseExpr(p, POWER_SET, false);

                nodesPush(&structt->as.structt.fields, assign);
                if (lexerExpect(&p->lexer, TOKEN_COMMA, TOKEN_RBRACE).kind == TOKEN_RBRACE) {
                    break;
                }
            }

            node = structt;
        } break;

        case TOKEN_LBRACKET: {
            Node *index = nodeAlloc(p->nodeAlloc, NODE_INDEX, token);
            index->as.index.base = node;

            if (lexerRead(&p->lexer, TOKEN_RANGE)) {
                token = p->lexer.buffer;
                assert(token.kind == TOKEN_RANGE);
            } else {
                index->as.index.at = parseExpr(p, POWER_SET, false);
                token = lexerExpect(&p->lexer, TOKEN_RANGE, TOKEN_RBRACKET);
            }

            if (token.kind == TOKEN_RANGE) {
                index->as.index.isRanged = true;
                if (!lexerRead(&p->lexer, TOKEN_RBRACKET)) {
                    index->as.index.end = parseExpr(p, POWER_SET, false);
                    lexerExpect(&p->lexer, TOKEN_RBRACKET);
                }
            }

            node = index;
        } break;

        default: {
            Node *binary = nodeAlloc(p->nodeAlloc, NODE_BINARY, token);
            binary->as.binary.lhs = node;
            binary->as.binary.rhs = parseExpr(p, lbp, noStruct);
            node = binary;

            if (token.kind == TOKEN_SET) {
                return node;
            }
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

static_assert(COUNT_TOKENS == 49, "");
static Node *parseStmt(Parser *p) {
    Node *node = NULL;

    Token token = lexerNext(&p->lexer);
    switch (token.kind) {
    case TOKEN_LBRACE:
        localAssert(p, token, true);
        node = nodeAlloc(p->nodeAlloc, NODE_BLOCK, token);
        while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
            nodesPush(&node->as.block, parseStmt(p));
        }

        assert(p->lexer.buffer.kind == TOKEN_RBRACE);
        node->token = p->lexer.buffer;
        break;

    case TOKEN_IF:
        localAssert(p, token, true);
        node = nodeAlloc(p->nodeAlloc, NODE_IF, token);
        node->as.iff.condition = parseExpr(p, POWER_SET, true);

        lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE));
        node->as.iff.consequence = parseStmt(p);

        if (lexerRead(&p->lexer, TOKEN_ELSE)) {
            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE, TOKEN_IF));
            node->as.iff.antecedence = parseStmt(p);
        }
        break;

    case TOKEN_FOR:
        localAssert(p, token, true);
        node = nodeAlloc(p->nodeAlloc, NODE_FOR, token);

        token = lexerPeek(&p->lexer);
        if (token.kind == TOKEN_VAR) {
            p->dontConsumeEols = true;
            node->as.forr.condition = parseStmt(p);
            p->dontConsumeEols = false;

            lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_EOL));
        } else if (token.kind != TOKEN_LBRACE) {
            node->as.forr.condition = parseExpr(p, POWER_NIL, true);

            Node *cond = node->as.forr.condition;
            if (cond->kind == NODE_BINARY && tokenKindPower(cond->token.kind) == POWER_SET) {
                lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_EOL));
            }
        }

        if (lexerRead(&p->lexer, TOKEN_EOL)) {
            consumeEols(p);
            node->as.forr.init = node->as.forr.condition;
            node->as.forr.condition = parseExpr(p, POWER_SET, true);

            if (lexerRead(&p->lexer, TOKEN_EOL)) {
                consumeEols(p);
                node->as.forr.update = parseExpr(p, POWER_NIL, true);
            }
        }

        lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_LBRACE));
        node->as.forr.body = parseStmt(p);
        break;

    case TOKEN_RETURN:
        localAssert(p, token, true);
        node = nodeAlloc(p->nodeAlloc, NODE_FLOW, token);

        token = lexerPeek(&p->lexer);
        if (!token.onNewline) {
            node->as.flow.operand = parseExpr(p, POWER_SET, false);
        }
        break;

    case TOKEN_FN:
        node = parseFn(p, lexerExpect(&p->lexer, TOKEN_IDENT));
        break;

    case TOKEN_VAR:
        node = nodeAlloc(p->nodeAlloc, NODE_VAR, lexerExpect(&p->lexer, TOKEN_IDENT));

        if (p->inExtern) {
            node->as.var.isExtern = true;
            node->as.var.type = parseType(p);
        } else {
            token = lexerPeek(&p->lexer);
            if (token.kind != TOKEN_SET) {
                node->as.var.type = parseType(p);
            }

            if (lexerRead(&p->lexer, TOKEN_SET)) {
                node->as.var.expr = parseExpr(p, POWER_SET, false);
            }
        }

        node->as.var.local = p->local;
        break;

    case TOKEN_TYPE:
        node = nodeAlloc(p->nodeAlloc, NODE_TYPE, lexerExpect(&p->lexer, TOKEN_IDENT));
        if (lexerRead(&p->lexer, TOKEN_SET)) {
            node->as.type.distinct = true;
        }
        node->as.type.definition = parseType(p);
        break;

    case TOKEN_EXTERN: {
        assert(!p->inExtern);
        node = nodeAlloc(p->nodeAlloc, NODE_EXTERN, token);

        p->inExtern = true;
        token = lexerExpect(&p->lexer, TOKEN_LBRACE, TOKEN_FN, TOKEN_VAR);
        if (token.kind == TOKEN_LBRACE) {
            while (!lexerRead(&p->lexer, TOKEN_RBRACE)) {
                lexerBuffer(&p->lexer, lexerExpect(&p->lexer, TOKEN_FN, TOKEN_VAR));
                nodesPush(&node->as.externn.definitions, parseStmt(p));
            }
        } else {
            lexerBuffer(&p->lexer, token);
            nodesPush(&node->as.externn.definitions, parseStmt(p));
        }
        p->inExtern = false;
    } break;

    case TOKEN_PRINT:
        localAssert(p, token, true);
        node = nodeAlloc(p->nodeAlloc, NODE_PRINT, token);
        node->as.print.operand = parseExpr(p, POWER_SET, false);
        break;

    default:
        if (!p->local && token.kind == TOKEN_IDENT) {
            if (!lexerRead(&p->lexer, TOKEN_WALRUS)) {
                localAssert(p, token, true);
            }

            node = nodeAlloc(p->nodeAlloc, NODE_VAR, token);
            node->as.var.expr = parseExpr(p, POWER_SET, false);
        } else {
            localAssert(p, token, true);
            lexerBuffer(&p->lexer, token);
            node = parseExpr(p, POWER_NIL, false);
        }
        break;
    }

    if (!p->dontConsumeEols) {
        consumeEols(p);
    }
    return node;
}

static Node *parseFn(Parser *p, Token name) {
    Node *node = nodeAlloc(p->nodeAlloc, NODE_FN, name);

    const bool localSave = p->local;
    p->local = true;

    {
        lexerExpect(&p->lexer, TOKEN_LPAREN);
        while (!lexerRead(&p->lexer, TOKEN_RPAREN)) {
            Node *arg = NULL;
            if (p->inExtern) {
                arg = parseArgWithOptionalName(p, node);
            } else {
                arg = nodeAlloc(p->nodeAlloc, NODE_ARG, lexerExpect(&p->lexer, TOKEN_IDENT));
                arg->as.arg.type = parseType(p);
                arg->as.arg.index = node->as.fn.arity++;
            }
            nodesPush(&node->as.fn.args, arg);

            if (lexerExpect(&p->lexer, TOKEN_COMMA, TOKEN_RPAREN).kind == TOKEN_RPAREN) {
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
    assert(p->nodeAlloc);

    p->lexer = lexer;
    while (!lexerRead(&p->lexer, TOKEN_EOF)) {
        consumeEols(p);
        if (lexerRead(&p->lexer, TOKEN_EOF)) {
            break;
        }

        nodesPush(&p->nodes, parseStmt(p));
    }
}
