#include <stdint.h>

#include "checker.h"

static Node *nodesFind(Nodes ns, Str name, Node *until) {
    for (Node *it = ns.head; it && it != until; it = it->next) {
        if (strEq(it->token.str, name)) {
            return it;
        }
    }

    return NULL;
}

// Scope
static void scopePush(Scope *s, Node *n) {
    if (s->length >= s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 128;
        s->data = realloc(s->data, s->capacity * sizeof(*s->data));
        assert(s->data);
    }

    s->data[s->length++] = n;
}

static FnContext fnContextBegin(Context *c, NodeFn *fn) {
    const FnContext save = c->fnContext;
    c->fnContext.base = c->locals.length;
    c->fnContext.fn = fn;
    return save;
}

static void fnContextEnd(Context *c, FnContext save) {
    c->locals.length = c->fnContext.base;
    c->fnContext = save;
}

static Node *fnContextFind(FnContext f, Scope s, Str name, bool isType) {
    assert(f.base <= s.length);
    s.data += f.base;
    s.length -= f.base;
    return scopeFind(s, name, isType);
}

static Node *identFind(Context *c, Str name, bool isType) {
    if (c->fnContext.fn) {
        Node *local = fnContextFind(c->fnContext, c->locals, name, isType);
        if (local && (local->kind == NODE_TYPE) == isType) {
            return local;
        }
    }

    return scopeFind(c->globals, name, isType);
}

static size_t blockBegin(Context *c) {
    return c->locals.length;
}

static void blockRestore(Context *c, size_t save) {
    c->locals.length = save;
}

// Checker
static_assert(COUNT_NODES == 21, "");
static void castUntypedInt(Node *n, Type expected) {
    switch (n->kind) {
    case NODE_ATOM:
        switch (n->token.kind) {
        case TOKEN_INT: {
            n->type = expected;

            static_assert(COUNT_TYPES == 17, "");
            const size_t intLimits[COUNT_TYPES] = {
                [TYPE_I8] = INT8_MAX,
                [TYPE_I16] = INT16_MAX,
                [TYPE_I32] = INT32_MAX,
                [TYPE_I64] = INT64_MAX,

                [TYPE_U8] = UINT8_MAX,
                [TYPE_U16] = UINT16_MAX,
                [TYPE_U32] = UINT32_MAX,
                [TYPE_U64] = UINT64_MAX,

                [TYPE_INT] = INT64_MAX,
            };

            const Type resolved = typeResolve(n->type);
            if (n->token.as.integer > intLimits[resolved.kind]) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Integer literal '" StrFmt "' is too large for type '%s'\n",
                    PosArg(n->token.pos),
                    StrArg(n->token.str),
                    typeToString(n->type));

                exit(1);
            }
        } break;

        case TOKEN_IDENT:
            castUntypedInt(n->as.atom.definition, expected);
            n->type = expected;
            break;

        default:
            unreachable();
        }
        break;

    case NODE_UNARY:
        castUntypedInt(n->as.unary.operand, expected);
        n->type = expected;
        break;

    case NODE_BINARY:
        castUntypedInt(n->as.binary.lhs, expected);
        castUntypedInt(n->as.binary.rhs, expected);
        n->type = expected;
        break;

    case NODE_VAR: {
        Node *expr = n->as.var.expr;
        assert(expr);

        castUntypedInt(expr, expected);
        n->type = expr->type;
    } break;

    case NODE_FLOW: {
        assert(n->token.kind == TOKEN_RETURN);

        Node *operand = n->as.flow.operand;
        assert(operand);

        castUntypedInt(operand, expected);
        n->type = operand->type;
    } break;

    default:
        unreachable();
    }
}

static bool tryAutoCastUntypedInt(Node *n, Type expected) {
    // If the types are already equal, consider it a succesful auto cast
    if (typeEq(n->type, expected)) {
        return true;
    }

    // Untyped integer -> Typed integer
    if (typeIsInteger(typeRemoveRef(expected)) && n->type.kind == TYPE_INT) {
        // The indirection level of the typed and untyped integers must match
        if (expected.ref != n->type.ref) {
            return false;
        }

        if (expected.kind != TYPE_INT) {
            castUntypedInt(n, expected);
        }

        return true;
    }

    return false;
}

static Type typeAssert(Node *n, Type expected) {
    if (!typeEq(n->type, expected)) {
        if (tryAutoCastUntypedInt(n, expected)) {
            return expected;
        }

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

static Type typeAssertNode(Node *a, Node *b) {
    if (typeEq(a->type, b->type)) {
        return a->type;
    }

    if (tryAutoCastUntypedInt(b, a->type)) {
        return a->type;
    }

    if (tryAutoCastUntypedInt(a, b->type)) {
        return b->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(a->token.pos),
        typeToString(b->type),
        typeToString(a->type));

    exit(1);
}

static Type typeAssertArith(const Node *n) {
    if (!typeIsInteger(n->type) && !typeIsPointer(n->type)) {
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
    if (typeIsInteger(n->type) || typeIsPointer(n->type)) {
        return n->type;
    }

    const Type resolved = typeResolve(n->type);
    if (resolved.kind == TYPE_BOOL || resolved.kind == TYPE_FN) {
        return n->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected scalar type, got '%s'\n",
        PosArg(n->token.pos),
        typeToString(n->type));

    exit(1);
}

static bool isTypeCastIllegal(Node *fromNode, Node *toNode) {
    const Type from = typeResolve(fromNode->type);
    const Type to = typeResolve(toNode->type);

    // Function -> Not rawptr
    if (from.kind == TYPE_FN) {
        return !typeEq(to, (Type) {.kind = TYPE_RAWPTR});
    }

    // Not rawptr -> Function
    if (to.kind == TYPE_FN) {
        return !typeEq(from, (Type) {.kind = TYPE_RAWPTR});
    }

    // Not 64 Bit Integer -> Pointer
    if (!typeIsPointer(from) && typeIsPointer(to)) {
        return !tryAutoCastUntypedInt(fromNode, (Type) {.kind = TYPE_U64});
    }

    // Pointer -> Not 64 Bit Integer
    if (!typeIsPointer(to) && typeIsPointer(from)) {
        return !tryAutoCastUntypedInt(toNode, (Type) {.kind = TYPE_U64});
    }

    return false;
}

static void errorUndefined(const Node *n, const char *label) {
    fprintf(
        stderr,
        PosFmt "ERROR: Undefined %s '" StrFmt "'\n",
        PosArg(n->token.pos),
        label,
        StrArg(n->token.str));

    exit(1);
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

static void checkConst(Context *c, Node *n) {
    unused(c);
    assert(n->kind == NODE_ATOM && n->token.kind == TOKEN_INT); // TODO: Proper constant evaluation
    n->type = (Type) {.kind = TYPE_INT};
}

static_assert(COUNT_TYPES == 17, "");
static void checkType(Context *c, Node *n) {
    switch (n->kind) {
    case NODE_ATOM:
        if (strMatch(n->token.str, "bool")) {
            n->type = (Type) {.kind = TYPE_BOOL};
        } else if (strMatch(n->token.str, "rawptr")) {
            n->type = (Type) {.kind = TYPE_RAWPTR};
        } else if (strMatch(n->token.str, "i8")) {
            n->type = (Type) {.kind = TYPE_I8};
        } else if (strMatch(n->token.str, "i16")) {
            n->type = (Type) {.kind = TYPE_I16};
        } else if (strMatch(n->token.str, "i32")) {
            n->type = (Type) {.kind = TYPE_I32};
        } else if (strMatch(n->token.str, "i64")) {
            n->type = (Type) {.kind = TYPE_I64};
        } else if (strMatch(n->token.str, "u8")) {
            n->type = (Type) {.kind = TYPE_U8};
        } else if (strMatch(n->token.str, "u16")) {
            n->type = (Type) {.kind = TYPE_U16};
        } else if (strMatch(n->token.str, "u32")) {
            n->type = (Type) {.kind = TYPE_U32};
        } else if (strMatch(n->token.str, "u64")) {
            n->type = (Type) {.kind = TYPE_U64};
        } else {
            Node *definition = identFind(c, n->token.str, true);
            if (!definition) {
                errorUndefined(n, "type");
            }

            n->type = definition->type;
        }
        break;

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;
        checkType(c, operand);
        n->type = operand->type;
        n->type.ref++;
    } break;

    case NODE_ARRAY: {
        Node *base = n->as.array.base;
        checkType(c, base);

        Node *arraySize = n->as.array.length;
        if (arraySize) {
            checkConst(c, arraySize);
            typeAssert(arraySize, (Type) {.kind = TYPE_U64});

            // TODO: Proper constant evaluation
            assert(arraySize->kind == NODE_ATOM && arraySize->token.kind == TOKEN_INT);
            n->as.array.lengthComputed = arraySize->token.as.integer;

            n->type = (Type) {.kind = TYPE_ARRAY, .spec = n};
        } else {
            n->type = (Type) {.kind = TYPE_SLICE, .spec = base};
        }
    } break;

    case NODE_FN:
        for (Node *it = n->as.fn.args.head; it; it = it->next) {
            if (it->token.kind == TOKEN_IDENT) {
                const Node *previous = nodesFind(n->as.fn.args, it->token.str, it);
                if (previous) {
                    errorRedefinition(it, previous, "argument");
                }
            }

            checkType(c, it->as.arg.type);
            it->type = it->as.arg.type->type;
        }

        if (n->as.fn.ret) {
            checkType(c, n->as.fn.ret);
        }

        n->type = (Type) {.kind = TYPE_FN, .spec = n};
        break;

    case NODE_STRUCT:
        assert(!n->as.structt.literalType);

        for (Node *it = n->as.structt.fields.head; it; it = it->next) {
            const Node *previous = nodesFind(n->as.structt.fields, it->token.str, it);
            if (previous) {
                errorRedefinition(it, previous, "field");
            }

            checkType(c, it->as.field.type);
            it->type = it->as.field.type->type;
        }

        n->type = (Type) {.kind = TYPE_STRUCT, .spec = n};
        break;

    default:
        unreachable();
    }
}

static void refPrevent(Node *n, bool ref) {
    if (ref) {
        fprintf(
            stderr,
            PosFmt "ERROR: Cannot take reference to value not in memory\n",
            PosArg(n->token.pos));

        exit(1);
    }
}

static void checkFn(Context *c, Node *n);

static Node *createTemp(Context *c, Node *expr, Type type) {
    Node *tempVar = nodeAlloc(c->nodeAlloc, NODE_VAR, (Token) {0});
    tempVar->type = type;
    tempVar->as.var.expr = expr;
    tempVar->as.var.local = true;

    if (c->fnContext.fn) {
        scopePush(&c->fnContext.fn->locals, tempVar);
    } else {
        scopePush(&c->globalTemps, tempVar);
    }

    return tempVar;
}

static_assert(COUNT_NODES == 21, "");
static void checkExpr(Context *c, Node *n, bool ref) {
    bool allowRef = false;

    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 44, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_IDENT: {
            Node *definition = identFind(c, n->token.str, false);
            if (definition) {
                n->as.atom.definition = definition;
                n->type = definition->type;

                allowRef = definition->kind == NODE_ARG || definition->kind == NODE_VAR;
                if (definition->kind == NODE_ARG && ref) {
                    definition->as.arg.memory = true;
                }
            } else {
                errorUndefined(n, "identifier");
            }
        } break;

        default:
            unreachable();
        }
        break;

    case NODE_CALL: {
        Node *fn = n->as.call.fn;
        checkExpr(c, fn, false);

        const Type resolved = typeResolve(fn->type);
        if (resolved.kind != TYPE_FN) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot call type '%s'\n",
                PosArg(fn->token.pos),
                typeToString(fn->type));

            exit(1);
        }

        if (resolved.ref != 0) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot call type '%s' without dereferencing it first\n",
                PosArg(fn->token.pos),
                typeToString(fn->type));

            exit(1);
        }

        const NodeCall actual = n->as.call;
        const NodeFn   expected = resolved.spec->as.fn;
        if (actual.arity != expected.arity) {
            fprintf(
                stderr,
                PosFmt "ERROR: Expected %zu argument%s, got %zu\n",
                PosArg(n->token.pos),
                expected.arity,
                expected.arity == 1 ? "" : "s",
                actual.arity);

            exit(1);
        }

        for (Node *a = actual.args.head, *e = expected.args.head; a; a = a->next, e = e->next) {
            checkExpr(c, a, false);
            typeAssertNode(a, e);
        }

        n->type = nodeFnReturnType(&resolved.spec->as.fn);
    } break;

    case NODE_CAST: {
        Node *from = n->as.cast.from;
        checkExpr(c, from, false);

        Node *to = n->as.cast.to;
        checkType(c, to);

        const Type fromType = typeAssertScalar(from);
        const Type toType = typeAssertScalar(to);
        if (!typeEq(fromType, toType) && isTypeCastIllegal(from, to)) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot cast type '%s' to type '%s'\n",
                PosArg(n->token.pos),
                typeToString(fromType),
                typeToString(toType));

            exit(1);
        }

        n->type = toType;
    } break;

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        static_assert(COUNT_TOKENS == 44, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            checkExpr(c, operand, false);
            n->type = typeAssertArith(operand);
            break;

        case TOKEN_MUL:
            checkExpr(c, operand, false);
            if (typeEq(operand->type, (Type) {.kind = TYPE_RAWPTR})) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Cannot dereference raw pointer\n",
                    PosArg(operand->token.pos));

                exit(1);
            }

            if (!typeIsPointer(operand->type)) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Expected pointer type, got '%s'\n",
                    PosArg(operand->token.pos),
                    typeToString(operand->type));

                exit(1);
            }

            n->type = operand->type;
            n->type.ref--;

            allowRef = true;
            break;

        case TOKEN_BAND:
            checkExpr(c, operand, true);
            n->type = operand->type;
            n->type.ref++;
            break;

        case TOKEN_BNOT:
            checkExpr(c, operand, false);
            n->type = typeAssertArith(operand);
            break;

        case TOKEN_LNOT:
            checkExpr(c, operand, false);
            n->type = typeAssert(operand, (Type) {.kind = TYPE_BOOL});
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_ARRAY: {
        todo();
    } break;

    case NODE_INDEX: {
        Node *base = n->as.index.base;
        Node *at = n->as.index.at;
        Node *end = n->as.index.end;

        checkExpr(c, base, false);
        const Type sliceType = typeResolve(base->type);

        if (end) {
            // Ranged: The "slice" can be an array or a slice or a pointer
            if (!typeIsPointer(base->type) && sliceType.kind != TYPE_ARRAY &&
                sliceType.kind != TYPE_SLICE) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Expected array or slice or pointer, got '%s'\n",
                    PosArg(base->token.pos),
                    typeToString(base->type));

                exit(1);
            }

            if (typeEq(sliceType, (Type) {.kind = TYPE_RAWPTR})) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Cannot construct slice from raw pointer\n",
                    PosArg(base->token.pos));

                exit(1);
            }
        } else {
            // Index: The "slice" can be an array or a slice
            if (sliceType.kind != TYPE_ARRAY && sliceType.kind != TYPE_SLICE) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Expected array or slice, got '%s'\n",
                    PosArg(base->token.pos),
                    typeToString(base->type));

                exit(1);
            }
        }

        if ((sliceType.kind == TYPE_ARRAY || sliceType.kind == TYPE_SLICE) && sliceType.ref) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot index type '%s' without dereferencing it first\n",
                PosArg(base->token.pos),
                typeToString(base->type));

            exit(1);
        }

        checkExpr(c, at, false);
        typeAssert(at, (Type) {.kind = TYPE_U64});

        if (end) {
            checkExpr(c, end, false);
            typeAssert(end, (Type) {.kind = TYPE_U64});

            if (sliceType.kind == TYPE_ARRAY) {
                assert(sliceType.spec);
                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec = sliceType.spec->as.array.base,
                };
            } else if (sliceType.kind == TYPE_SLICE) {
                n->type = base->type;
            } else {
                Type elementType;
                if (base->type.ref) {
                    elementType = base->type;
                } else {
                    elementType = sliceType;
                }
                elementType.ref--;

                Node *elementNode = nodeAlloc(c->nodeAlloc, NODE_ATOM, (Token) {0});
                elementNode->type = elementType;
                n->type = (Type) {.kind = TYPE_SLICE, .spec = elementNode};
            }
        } else {
            assert(sliceType.spec);
            if (sliceType.kind == TYPE_ARRAY) {
                n->type = sliceType.spec->as.array.base->type;
            } else {
                n->type = sliceType.spec->type;
            }

            allowRef = true;
        }
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 44, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            checkExpr(c, lhs, false);
            checkExpr(c, rhs, false);
            typeAssertArith(lhs);
            n->type = typeAssertNode(rhs, lhs);
            break;

        case TOKEN_SHL:
        case TOKEN_SHR:
        case TOKEN_BOR:
        case TOKEN_BAND:
            checkExpr(c, lhs, false);
            checkExpr(c, rhs, false);
            typeAssertArith(lhs);
            n->type = typeAssertNode(rhs, lhs);
            break;

        case TOKEN_SET:
            checkExpr(c, lhs, true);
            checkExpr(c, rhs, false);
            typeAssertNode(rhs, lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        case TOKEN_LOR:
        case TOKEN_LAND:
            checkExpr(c, lhs, false);
            checkExpr(c, rhs, false);
            n->type = typeAssert(rhs, typeAssert(lhs, (Type) {.kind = TYPE_BOOL}));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            checkExpr(c, lhs, false);
            checkExpr(c, rhs, false);
            typeAssertArith(lhs);
            typeAssertNode(rhs, lhs);
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_MEMBER: {
        Node *lhs = n->as.member.lhs;
        Node *rhs = n->as.member.rhs;

        switch (lhs->kind) {
        case NODE_CALL:
        case NODE_INDEX:
            checkExpr(c, lhs, false);
            break;

        case NODE_MEMBER:
            checkExpr(c, lhs, false);
            n->as.member.isTemporary = lhs->as.member.isTemporary;
            allowRef = !n->as.member.isTemporary;
            break;

        case NODE_STRUCT:
            checkExpr(c, lhs, false);
            break;

        default:
            checkExpr(c, lhs, true);
            allowRef = true;
            break;
        }

        const Type lhsType = typeResolve(lhs->type);
        if (lhsType.kind != TYPE_STRUCT && lhsType.kind != TYPE_ARRAY &&
            lhsType.kind != TYPE_SLICE) {
            fprintf(
                stderr,
                PosFmt "ERROR: Expected array or slice or structure, got '%s'\n",
                PosArg(lhs->token.pos),
                typeToString(lhs->type));

            exit(1);
        }

        bool allocTemp = false;
        allocTemp = allocTemp || (lhs->kind == NODE_CALL && lhsType.ref == 0);
        allocTemp = allocTemp || (lhs->kind == NODE_INDEX && lhsType.kind == TYPE_SLICE);
        if (allocTemp) {
            n->as.member.isTemporary = true;
            n->as.member.lhs = createTemp(c, lhs, lhs->type);
        }

        if (lhsType.kind == TYPE_STRUCT) {
            const NodeStruct structt = lhsType.spec->as.structt;

            Node *definition = nodesFind(structt.fields, rhs->token.str, NULL);
            if (!definition) {
                errorUndefined(rhs, "field");
            }

            rhs->as.atom.definition = definition;
            n->type = definition->type;
        } else if (lhsType.kind == TYPE_ARRAY || lhsType.kind == TYPE_SLICE) {
            if (strMatch(rhs->token.str, "data")) {
                rhs->token.as.integer = 0;
                n->type = lhsType.spec->type;
                n->type.ref++;
            } else if (strMatch(rhs->token.str, "length")) {
                rhs->token.as.integer = 1;
                n->type = (Type) {.kind = TYPE_U64};
            } else {
                errorUndefined(rhs, "field");
            }
        } else {
            unreachable();
        }
    } break;

    case NODE_SIZEOF: {
        Node *operand = n->as.sizeoff.operand;
        if (n->as.sizeoff.isExpr) {
            checkExpr(c, operand, false);
        } else {
            checkType(c, operand);
        }

        n->type = (Type) {.kind = TYPE_U64};
    } break;

    case NODE_FN:
        checkFn(c, n);
        break;

    case NODE_STRUCT: {
        Node *lhs = n->as.structt.literalType;
        assert(lhs);
        checkType(c, lhs);

        const Type lhsType = typeResolve(lhs->type);
        if (lhsType.kind != TYPE_STRUCT) {
            fprintf(
                stderr,
                PosFmt "ERROR: Expected structure, got '%s'\n",
                PosArg(lhs->token.pos),
                typeToString(lhs->type));

            exit(1);
        }

        const NodeStruct structt = lhsType.spec->as.structt;
        for (Node *it = n->as.structt.fields.head; it; it = it->next) {
            assert(it->kind == NODE_BINARY);
            Node *definition = nodesFind(structt.fields, it->token.str, NULL);
            if (!definition) {
                errorUndefined(it, "field");
            }

            it->as.binary.lhs = definition;
            checkExpr(c, it->as.binary.rhs, false);
            typeAssert(it->as.binary.rhs, definition->type);
        }

        n->as.structt.literalTemp = createTemp(c, NULL, lhs->type);
        n->type = lhs->type;
    } break;

    default:
        unreachable();
    }

    if (!allowRef) {
        refPrevent(n, ref);
    }
}

static_assert(COUNT_NODES == 21, "");
static bool alwaysReturns(Node *n) {
    switch (n->kind) {
    case NODE_CALL:
        // TODO: Introduce a "No Return" return type
        return false;

    case NODE_BLOCK:
        for (Node *it = n->as.block.head; it; it = it->next) {
            if (alwaysReturns(it)) {
                return true;
            }
        }
        return false;

    case NODE_IF:
        if (!n->as.iff.antecedence) {
            return false;
        }

        // TODO: Condition analysis
        return alwaysReturns(n->as.iff.consequence) && alwaysReturns(n->as.iff.antecedence);

    case NODE_FOR: {
        if (n->as.forr.init && alwaysReturns(n->as.forr.init)) {
            return true;
        }

        Node *cond = n->as.forr.condition;
        bool  infinite = false;

        if (!cond) {
            infinite = true;
        } else if (
            // TODO: Constant evaluation
            cond->kind == NODE_ATOM && cond->token.kind == TOKEN_BOOL && cond->token.as.boolean) {
            infinite = true;
        }

        if (infinite) {
            // Till we get break, an infinite loop "always returns"
            return true;
        }

        return false;
    }

    case NODE_FLOW:
        static_assert(COUNT_TOKENS == 44, "");
        switch (n->token.kind) {
        case TOKEN_RETURN:
            return true;

        default:
            unreachable();
        }

    default:
        return false;
    }
}

static_assert(COUNT_NODES == 21, "");
static void checkStmt(Context *c, Node *n) {
    switch (n->kind) {
    case NODE_BLOCK: {
        const size_t blockSave = blockBegin(c);
        for (Node *it = n->as.block.head; it; it = it->next) {
            checkStmt(c, it);
        }
        blockRestore(c, blockSave);
    } break;

    case NODE_IF:
        checkExpr(c, n->as.iff.condition, false);
        typeAssert(n->as.iff.condition, (Type) {.kind = TYPE_BOOL});

        checkStmt(c, n->as.iff.consequence);
        if (n->as.iff.antecedence) {
            checkStmt(c, n->as.iff.antecedence);
        }
        break;

    case NODE_FOR: {
        const size_t blockSave = blockBegin(c);
        if (n->as.forr.init) {
            checkStmt(c, n->as.forr.init);
        }

        if (n->as.forr.condition) {
            checkExpr(c, n->as.forr.condition, false);
            typeAssert(n->as.forr.condition, (Type) {.kind = TYPE_BOOL});
        }

        if (n->as.forr.update) {
            checkStmt(c, n->as.forr.update);
        }

        checkStmt(c, n->as.forr.body);
        blockRestore(c, blockSave);
    } break;

    case NODE_FLOW: {
        Node *operand = n->as.flow.operand;

        static_assert(COUNT_TOKENS == 44, "");
        switch (n->token.kind) {
        case TOKEN_RETURN: {
            n->type = (Type) {.kind = TYPE_UNIT};
            if (operand) {
                checkExpr(c, operand, false);
                n->type = operand->type;
            }

            typeAssert(n, nodeFnReturnType(c->fnContext.fn));
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_FN:
        checkFn(c, n);
        break;

    case NODE_ARG:
        checkType(c, n->as.arg.type);
        n->type = n->as.arg.type->type;

        if (!c->inExtern) {
            scopePush(&c->locals, n);
            scopePush(&c->fnContext.fn->locals, n);
        }
        break;

    case NODE_VAR:
        if (!n->as.var.local) {
            const Node *previous = scopeFind(c->globals, n->token.str, false);
            if (previous) {
                errorRedefinition(n, previous, "identifier");
            }
        }

        if (n->as.var.type) {
            checkType(c, n->as.var.type);
            n->type = n->as.var.type->type;
        }

        Node *expr = n->as.var.expr;
        if (expr) {
            checkExpr(c, expr, false);
            n->type = expr->type;

            if (n->type.kind == TYPE_UNIT) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Cannot define variable with type '%s'\n",
                    PosArg(n->token.pos),
                    typeToString(n->type));

                exit(1);
            }

            if (n->as.var.type) {
                typeAssert(expr, n->as.var.type->type);
                n->type = expr->type;
            }
        }

        if (n->as.var.local) {
            scopePush(&c->locals, n);
            scopePush(&c->fnContext.fn->locals, n);
        } else {
            scopePush(&c->globals, n);
        }
        break;

    case NODE_TYPE:
        if (!c->fnContext.fn) {
            const Node *previous = scopeFind(c->globals, n->token.str, true);
            if (previous) {
                errorRedefinition(n, previous, "type");
            }
        }

        checkType(c, n->as.type.definition);
        n->as.type.real = typeResolve(n->as.type.definition->type);
        n->type = (Type) {.kind = TYPE_ALIAS, .spec = n};

        if (c->fnContext.fn) {
            scopePush(&c->locals, n);
        } else {
            scopePush(&c->globals, n);
        }
        break;

    case NODE_EXTERN:
        c->inExtern = true;
        for (Node *it = n->as.externn.definitions.head; it; it = it->next) {
            checkStmt(c, it);
        }
        c->inExtern = false;
        break;

    case NODE_PRINT:
        checkExpr(c, n->as.print.operand, false);
        typeAssertScalar(n->as.print.operand);
        break;

    default:
        checkExpr(c, n, false);
        break;
    }
}

static void checkFn(Context *c, Node *n) {
    if (n->token.kind == TOKEN_IDENT) {
        if (c->fnContext.fn) {
            scopePush(&c->locals, n);
        } else {
            const Node *previous = scopeFind(c->globals, n->token.str, false);
            if (previous) {
                errorRedefinition(n, previous, "identifier");
            }

            scopePush(&c->globals, n);
        }
    }

    n->type = (Type) {.kind = TYPE_FN, .spec = n};

    {
        const FnContext fnContextSave = fnContextBegin(c, &n->as.fn);
        for (Node *it = n->as.fn.args.head; it; it = it->next) {
            const Node *previous = nodesFind(n->as.fn.args, it->token.str, it);
            if (previous) {
                errorRedefinition(it, previous, "argument");
            }

            checkStmt(c, it);
        }

        if (n->as.fn.ret) {
            checkType(c, n->as.fn.ret);
        }

        if (!c->inExtern) {
            checkStmt(c, n->as.fn.body);
            if (n->as.fn.ret && !alwaysReturns(n->as.fn.body)) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Expected return statement\n",
                    PosArg(n->as.fn.body->token.pos));

                exit(1);
            }
        }

        fnContextEnd(c, fnContextSave);
    }
}

void checkNodes(Context *c, Nodes nodes) {
    assert(c->nodeAlloc);
    for (Node *it = nodes.head; it; it = it->next) {
        checkStmt(c, it);
    }
}
