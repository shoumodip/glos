#include "compiler.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

// NOTE: This is temporary
#include <llvm-c/Analysis.h>

typedef struct {
    LLVMValueRef      fn;
    LLVMBasicBlockRef block;
} FnCompiler;

typedef struct {
    Context    context;
    FnCompiler fnCompiler;

    LLVMModuleRef  module;
    LLVMBuilderRef builder;

    LLVMTypeRef sliceType;

    // The 'print' keyword
    LLVMTypeRef  printSFmtType;
    LLVMValueRef printSFmtValue;
    LLVMTypeRef  printUFmtType;
    LLVMValueRef printUFmtValue;
    LLVMTypeRef  printFuncType;
    LLVMValueRef printFuncValue;
} Compiler;

static FnCompiler fnCompilerBegin(Compiler *c, Node *fn) {
    c->fnCompiler.block = LLVMGetInsertBlock(c->builder);
    const FnCompiler save = c->fnCompiler;
    c->fnCompiler.fn = fn->as.fn.llvm;
    return save;
}

static void fnCompilerEnd(Compiler *c, FnCompiler save) {
    c->fnCompiler = save;
    LLVMPositionBuilderAtEnd(c->builder, save.block);
}

// How the type should actually be represented in memory.
// Eg: fn () -> ptr
static LLVMTypeRef typeInMemory(Type type) {
    if (type.kind == TYPE_FN) {
        return LLVMPointerType(LLVMVoidType(), 0);
    }
    return type.llvm;
}

static_assert(COUNT_TYPES == 17, "");
static void compileType(Compiler *c, Type *type) {
    if (typeIsPointer(*type)) {
        type->llvm = LLVMPointerType(LLVMVoidType(), 0);
        return;
    }

    switch (type->kind) {
    case TYPE_UNIT:
        type->llvm = LLVMVoidType();
        break;

    case TYPE_BOOL:
        type->llvm = LLVMInt1Type();
        break;

    case TYPE_I8:
    case TYPE_U8:
        type->llvm = LLVMInt8Type();
        break;

    case TYPE_I16:
    case TYPE_U16:
        type->llvm = LLVMInt16Type();
        break;

    case TYPE_I32:
    case TYPE_U32:
        type->llvm = LLVMInt32Type();
        break;

    case TYPE_I64:
    case TYPE_U64:
    case TYPE_INT:
        type->llvm = LLVMInt64Type();
        break;

    case TYPE_FN: {
        assert(type->spec);
        NodeFn *fn = &type->spec->as.fn;

        LLVMTypeRef *argsLLVM = calloc(fn->arity, sizeof(LLVMTypeRef));
        for (Node *it = fn->args.head; it; it = it->next) {
            compileType(c, &it->type);
            argsLLVM[it->as.arg.index] = typeInMemory(it->type);
        }

        LLVMTypeRef returnType = NULL;
        if (fn->ret) {
            compileType(c, &fn->ret->type);
            returnType = typeInMemory(fn->ret->type);
        } else {
            returnType = LLVMVoidType();
        }

        type->llvm = LLVMFunctionType(returnType, argsLLVM, fn->arity, false);
        free(argsLLVM);
    } break;

    case TYPE_ARRAY: {
        assert(type->spec);
        NodeArray *array = &type->spec->as.array;

        compileType(c, &array->base->type);
        type->llvm = LLVMArrayType2(typeInMemory(array->base->type), array->lengthComputed);
    } break;

    case TYPE_SLICE:
        type->llvm = c->sliceType;
        break;

    case TYPE_STRUCT: {
        assert(type->spec);
        NodeStruct *structt = &type->spec->as.structt;

        LLVMTypeRef *fieldsLLVM = calloc(structt->fieldsCount, sizeof(LLVMTypeRef));
        for (Node *it = structt->fields.head; it; it = it->next) {
            compileType(c, &it->type);
            fieldsLLVM[it->as.field.index] = typeInMemory(it->type);
        }

        type->llvm = LLVMStructType(fieldsLLVM, structt->fieldsCount, false);
        free(fieldsLLVM);
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 21, "");
static LLVMValueRef definitionLLVMValue(Node *n) {
    switch (n->kind) {
    case NODE_FN:
        return n->as.fn.llvm;

    case NODE_ARG:
        return n->as.arg.llvm;

    case NODE_VAR:
        return n->as.var.llvm;

    default:
        unreachable();
    }
}

// LLVM cannot perform arithmetic operations on pointers directly
static void beforeArith(Compiler *c, Type type, LLVMValueRef *lhs, LLVMValueRef *rhs) {
    if (!typeIsPointer(type)) {
        return;
    }

    LLVMTypeRef  typeTemp = LLVMInt64Type();
    LLVMValueRef lhsTemp = LLVMBuildPtrToInt(c->builder, *lhs, typeTemp, "");
    LLVMValueRef rhsTemp = LLVMBuildPtrToInt(c->builder, *rhs, typeTemp, "");

    *lhs = lhsTemp;
    *rhs = rhsTemp;
}

static LLVMValueRef afterArith(Compiler *c, Type type, LLVMValueRef result) {
    if (!typeIsPointer(type)) {
        return result;
    }

    return LLVMBuildIntToPtr(c->builder, result, type.llvm, "");
}

static void compileFn(Compiler *c, Node *n);
static void compileStmt(Compiler *c, Node *n);

static void encodeString(char *buffer, Str str) {
    str.data++;
    str.length--;

    for (size_t i = 0; i < str.length; i++) {
        char ch = str.data[i];
        if (ch == '\\') {
            ch = str.data[++i];
            resolveEscapeChar(&ch);
        }

        *buffer++ = ch;
    }
}

static_assert(COUNT_NODES == 21, "");
static LLVMValueRef compileExpr(Compiler *c, Node *n, bool ref) {
    compileType(c, &n->type);

    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 47, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_CHAR:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, true);

        case TOKEN_BOOL:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, false);

        case TOKEN_STR: {
            const size_t length = n->token.as.integer;

            char *buffer = tempAlloc(length);
            encodeString(buffer, n->token.str);

            LLVMTypeRef  strType = LLVMArrayType2(LLVMInt8Type(), length + 1);
            LLVMValueRef strGlobal = LLVMAddGlobal(c->module, strType, "");
            LLVMSetInitializer(strGlobal, LLVMConstString(buffer, length, false));

            LLVMValueRef strValue = LLVMGetUndef(c->sliceType);
            strValue = LLVMBuildInsertValue(c->builder, strValue, strGlobal, 0, "");

            LLVMValueRef lengthValue = LLVMConstInt(LLVMInt64Type(), length, false);
            strValue = LLVMBuildInsertValue(c->builder, strValue, lengthValue, 1, "");

            tempReset(buffer);
            return strValue;
        }

        case TOKEN_IDENT: {
            Node *definition = n->as.atom.definition;
            if (definition->kind == NODE_ARG) {
                NodeArg arg = definition->as.arg;
                if (!arg.memory) {
                    return LLVMGetParam(c->fnCompiler.fn, arg.index);
                }
            }

            LLVMValueRef definitionLLVM = definitionLLVMValue(definition);
            if (ref || definition->kind == NODE_FN) {
                return definitionLLVM;
            }

            return LLVMBuildLoad2(c->builder, typeInMemory(n->type), definitionLLVM, "");
        };

        default:
            unreachable();
        }
        break;

    case NODE_CALL: {
        NodeCall call = n->as.call;

        Node        *fn = call.fn;
        LLVMValueRef fnValue = compileExpr(c, fn, false);

        LLVMValueRef *argsLLVM = calloc(call.arity, sizeof(LLVMValueRef));
        {
            size_t i = 0;
            for (Node *it = call.args.head; it; it = it->next) {
                argsLLVM[i++] = compileExpr(c, it, false);
            }
        }

        LLVMValueRef result =
            LLVMBuildCall2(c->builder, fn->type.llvm, fnValue, argsLLVM, call.arity, "");

        free(argsLLVM);
        return result;
    };

    case NODE_CAST: {
        Node              *from = n->as.cast.from;
        const LLVMValueRef fromValue = compileExpr(c, from, false);

        const Type fromType = from->type;
        const Type toType = n->type;
        if (typeEq(fromType, toType) || fromType.kind == TYPE_FN || toType.kind == TYPE_FN) {
            return fromValue;
        }

        const LLVMTypeRef toLLVM = typeInMemory(toType);
        if (typeIsPointer(fromType)) {
            if (typeIsPointer(toType)) {
                // Pointer -> Pointer
                return LLVMBuildBitCast(c->builder, fromValue, toLLVM, "");
            } else {
                // Pointer -> Integer
                return LLVMBuildPtrToInt(c->builder, fromValue, toLLVM, "");
            }
        }

        if (fromType.kind == TYPE_BOOL) {
            // Boolean -> Integer
            return LLVMBuildZExt(c->builder, fromValue, toLLVM, "");
        }

        static_assert(COUNT_TYPES == 17, "");
        const size_t intSizes[COUNT_TYPES] = {
            [TYPE_I8] = 8,
            [TYPE_I16] = 16,
            [TYPE_I32] = 32,
            [TYPE_I64] = 64,

            [TYPE_U8] = 8,
            [TYPE_U16] = 16,
            [TYPE_U32] = 32,
            [TYPE_U64] = 64,

            [TYPE_INT] = 64,
        };

        if (intSizes[fromType.kind]) {
            if (typeIsPointer(toType)) {
                // Integer -> Pointer
                return LLVMBuildIntToPtr(c->builder, fromValue, toLLVM, "");
            }

            if (toType.kind == TYPE_BOOL) {
                // Integer -> Boolean
                const LLVMValueRef zero = LLVMConstNull(typeInMemory(fromType));
                return LLVMBuildICmp(c->builder, LLVMIntNE, fromValue, zero, "");
            }

            if (intSizes[toType.kind]) {
                // Integer -> Integer
                const size_t fromSize = intSizes[fromType.kind];
                const size_t toSize = intSizes[toType.kind];
                if (fromSize == toSize) {
                    return fromValue;
                }

                if (fromSize > toSize) {
                    return LLVMBuildTrunc(c->builder, fromValue, toLLVM, "");
                }

                if (typeIsSigned(fromType)) {
                    return LLVMBuildSExt(c->builder, fromValue, toLLVM, "");
                } else {
                    return LLVMBuildZExt(c->builder, fromValue, toLLVM, "");
                }
            }
        }

        unreachable();
    }

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        static_assert(COUNT_TOKENS == 47, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            const LLVMValueRef operandValue = compileExpr(c, operand, false);
            return LLVMBuildNeg(c->builder, operandValue, "");
        };

        case TOKEN_MUL: {
            const LLVMValueRef operandValue = compileExpr(c, operand, false);
            if (ref) {
                return operandValue;
            }

            return LLVMBuildLoad2(c->builder, typeInMemory(n->type), operandValue, "");
        }

        case TOKEN_BAND:
            return compileExpr(c, operand, true);

        case TOKEN_BNOT: {
            const LLVMValueRef operandValue = compileExpr(c, operand, false);
            return LLVMBuildNot(c->builder, operandValue, "");
        }

        case TOKEN_LNOT: {
            const LLVMValueRef operandValue = compileExpr(c, operand, false);
            return LLVMBuildNot(c->builder, operandValue, "");
        }

        default:
            unreachable();
        }
    } break;

    case NODE_ARRAY: {
        Node *temp = n->as.array.literalTemp;
        assert(temp);

        LLVMValueRef tempValue = compileExpr(c, temp, false);

        compileType(c, &n->as.array.base->type);
        const LLVMTypeRef elementType = typeInMemory(n->as.array.base->type);

        for (Node *it = n->as.array.literalInits.head; it; it = it->next) {
            LLVMValueRef index = compileExpr(c, it->as.binary.lhs, false);
            LLVMValueRef pointer =
                LLVMBuildInBoundsGEP2(c->builder, elementType, tempValue, &index, 1, "");

            LLVMValueRef assign = compileExpr(c, it->as.binary.rhs, false);
            LLVMBuildStore(c->builder, assign, pointer);
        }

        if (ref) {
            return tempValue;
        }

        return LLVMBuildLoad2(c->builder, n->type.llvm, tempValue, "");
    }

    case NODE_INDEX: {
        Node *base = n->as.index.base;
        Node *at = n->as.index.at;
        Node *to = n->as.index.end;

        const bool   isArray = base->type.kind == TYPE_ARRAY && base->type.ref == 0;
        LLVMValueRef pointer = compileExpr(c, base, isArray);

        LLVMValueRef atValue = NULL;
        if (at) {
            atValue = compileExpr(c, at, false);
        } else {
            atValue = LLVMConstNull(LLVMInt64Type());
        }

        if (!n->as.index.isRanged) {
            if (!isArray) {
                pointer = LLVMBuildExtractValue(c->builder, pointer, 0, "");
            }

            const LLVMTypeRef elementType = typeInMemory(n->type);
            pointer = LLVMBuildInBoundsGEP2(c->builder, elementType, pointer, &atValue, 1, "");

            if (ref) {
                return pointer;
            }

            return LLVMBuildLoad2(c->builder, elementType, pointer, "");
        }

        LLVMValueRef toValue = NULL;
        if (!to) {
            assert(!base->type.ref);
            if (base->type.kind == TYPE_ARRAY) {
                assert(base->type.spec);
                toValue =
                    LLVMConstInt(LLVMInt64Type(), base->type.spec->as.array.lengthComputed, false);
            } else if (base->type.kind == TYPE_SLICE) {
                toValue = LLVMBuildExtractValue(c->builder, pointer, 1, "");
            } else {
                unreachable();
            }
        }

        if (base->type.kind == TYPE_SLICE) {
            pointer = LLVMBuildExtractValue(c->builder, pointer, 0, "");
        }

        assert(n->type.kind == TYPE_SLICE);
        assert(n->type.spec);

        Type *elementType = &n->type.spec->type;
        compileType(c, elementType);

        pointer =
            LLVMBuildInBoundsGEP2(c->builder, typeInMemory(*elementType), pointer, &atValue, 1, "");

        if (to) {
            toValue = compileExpr(c, to, false);
        }

        LLVMValueRef length = LLVMBuildSub(c->builder, toValue, atValue, "");
        LLVMValueRef sliceValue = LLVMGetUndef(c->sliceType);
        sliceValue = LLVMBuildInsertValue(c->builder, sliceValue, pointer, 0, "");
        sliceValue = LLVMBuildInsertValue(c->builder, sliceValue, length, 1, "");
        return sliceValue;
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 47, "");
        switch (n->token.kind) {
        case TOKEN_ADD: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);
            return afterArith(c, n->type, LLVMBuildAdd(c->builder, lhsValue, rhsValue, ""));
        }

        case TOKEN_SUB: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);
            return afterArith(c, n->type, LLVMBuildSub(c->builder, lhsValue, rhsValue, ""));
        }

        case TOKEN_MUL: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);
            return afterArith(c, n->type, LLVMBuildMul(c->builder, lhsValue, rhsValue, ""));
        }

        case TOKEN_DIV: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);

            LLVMValueRef result = NULL;
            if (typeIsSigned(n->type)) {
                result = LLVMBuildSDiv(c->builder, lhsValue, rhsValue, "");
            } else {
                result = LLVMBuildUDiv(c->builder, lhsValue, rhsValue, "");
            }
            return afterArith(c, n->type, result);
        }

        case TOKEN_SHL: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);
            return afterArith(c, n->type, LLVMBuildShl(c->builder, lhsValue, rhsValue, ""));
        }

        case TOKEN_SHR: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);

            LLVMValueRef result = NULL;
            if (typeIsSigned(n->type)) {
                result = LLVMBuildAShr(c->builder, lhsValue, rhsValue, "");
            } else {
                result = LLVMBuildLShr(c->builder, lhsValue, rhsValue, "");
            }
            return afterArith(c, n->type, result);
        }

        case TOKEN_BOR: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);
            return afterArith(c, n->type, LLVMBuildOr(c->builder, lhsValue, rhsValue, ""));
        }

        case TOKEN_BAND: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            beforeArith(c, n->type, &lhsValue, &rhsValue);
            return afterArith(c, n->type, LLVMBuildAnd(c->builder, lhsValue, rhsValue, ""));
        }

        case TOKEN_SET: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, true);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            return LLVMBuildStore(c->builder, rhsValue, lhsValue);
        }

        case TOKEN_LOR: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);

            LLVMBasicBlockRef currentBlock = LLVMGetInsertBlock(c->builder);
            LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
            LLVMBasicBlockRef finalBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
            LLVMBuildCondBr(c->builder, lhsValue, finalBlock, rhsBlock);

            // RHS
            LLVMPositionBuilderAtEnd(c->builder, rhsBlock);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            LLVMBuildBr(c->builder, finalBlock);

            // Finally
            LLVMPositionBuilderAtEnd(c->builder, finalBlock);

            LLVMValueRef result = LLVMBuildPhi(c->builder, n->type.llvm, "");
            LLVMValueRef trueLLVM = LLVMConstInt(n->type.llvm, true, false);
            LLVMAddIncoming(result, &trueLLVM, &currentBlock, 1);
            LLVMAddIncoming(result, &rhsValue, &rhsBlock, 1);
            return result;
        }

        case TOKEN_LAND: {
            LLVMValueRef lhsValue = compileExpr(c, lhs, false);

            LLVMBasicBlockRef currentBlock = LLVMGetInsertBlock(c->builder);
            LLVMBasicBlockRef rhsBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
            LLVMBasicBlockRef finalBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
            LLVMBuildCondBr(c->builder, lhsValue, rhsBlock, finalBlock);

            // RHS
            LLVMPositionBuilderAtEnd(c->builder, rhsBlock);
            LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            LLVMBuildBr(c->builder, finalBlock);

            // Finally
            LLVMPositionBuilderAtEnd(c->builder, finalBlock);

            LLVMValueRef result = LLVMBuildPhi(c->builder, n->type.llvm, "");
            LLVMValueRef falseLLVM = LLVMConstInt(n->type.llvm, false, false);
            LLVMAddIncoming(result, &falseLLVM, &currentBlock, 1);
            LLVMAddIncoming(result, &rhsValue, &rhsBlock, 1);
            return result;
        }

        case TOKEN_GT: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);

            const LLVMIntPredicate pred = typeIsSigned(lhs->type) ? LLVMIntSGT : LLVMIntUGT;
            return LLVMBuildICmp(c->builder, pred, lhsValue, rhsValue, "");
        }

        case TOKEN_GE: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);

            const LLVMIntPredicate pred = typeIsSigned(lhs->type) ? LLVMIntSGE : LLVMIntUGE;
            return LLVMBuildICmp(c->builder, pred, lhsValue, rhsValue, "");
        }

        case TOKEN_LT: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);

            const LLVMIntPredicate pred = typeIsSigned(lhs->type) ? LLVMIntSLT : LLVMIntULT;
            return LLVMBuildICmp(c->builder, pred, lhsValue, rhsValue, "");
        }

        case TOKEN_LE: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);

            const LLVMIntPredicate pred = typeIsSigned(lhs->type) ? LLVMIntSLE : LLVMIntULE;
            return LLVMBuildICmp(c->builder, pred, lhsValue, rhsValue, "");
        }

        case TOKEN_EQ: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            return LLVMBuildICmp(c->builder, LLVMIntEQ, lhsValue, rhsValue, "");
        }

        case TOKEN_NE: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            return LLVMBuildICmp(c->builder, LLVMIntNE, lhsValue, rhsValue, "");
        }

        default:
            unreachable();
        }
    } break;

    case NODE_MEMBER: {
        Node *lhs = n->as.member.lhs;
        Node *rhs = n->as.member.rhs;

        LLVMValueRef lhsValue = compileExpr(c, lhs, true);

        size_t index = 0;

        assert(rhs->kind == NODE_ATOM);
        if (lhs->type.kind == TYPE_STRUCT) {
            assert(rhs->as.atom.definition->kind == NODE_FIELD);
            index = rhs->as.atom.definition->as.field.index;
        } else if (lhs->type.kind == TYPE_SLICE) {
            index = rhs->token.as.integer;

            if ((lhs->kind == NODE_INDEX && lhs->as.index.isRanged) ||
                (lhs->kind == NODE_ATOM && lhs->token.kind == TOKEN_STR)) {
                return LLVMBuildExtractValue(c->builder, lhsValue, index, "");
            }
        } else if (lhs->type.kind == TYPE_ARRAY) {
            if (rhs->token.as.integer) {
                // Length
                assert(lhs->type.spec);
                return LLVMConstInt(n->type.llvm, lhs->type.spec->as.array.lengthComputed, false);
            } else {
                // Data
                return lhsValue;
            }
        } else {
            unreachable();
        }

        if (lhs->type.ref) {
            const LLVMTypeRef voidPtr = LLVMPointerType(LLVMVoidType(), 0);

            size_t count = lhs->type.ref;
            if (lhs->kind == NODE_CALL) {
                count--;
            }

            for (size_t i = 0; i < count; i++) {
                lhsValue = LLVMBuildLoad2(c->builder, voidPtr, lhsValue, "");
            }
        }

        LLVMTypeRef structTypeLLVM = NULL;
        if (lhs->type.kind == TYPE_STRUCT) {
            assert(lhs->type.spec->kind == NODE_STRUCT);

            Type *structType = &lhs->type.spec->type;
            if (!structType->llvm) {
                compileType(c, structType);
            }
            structTypeLLVM = structType->llvm;
        } else if (lhs->type.kind == TYPE_SLICE) {
            structTypeLLVM = c->sliceType;
        } else {
            unreachable();
        }

        LLVMValueRef fieldValue =
            LLVMBuildStructGEP2(c->builder, structTypeLLVM, lhsValue, index, "");

        if (ref) {
            return fieldValue;
        }

        return LLVMBuildLoad2(c->builder, typeInMemory(n->type), fieldValue, "");
    }

    case NODE_SIZEOF: {
        Node *operand = n->as.sizeoff.operand;
        compileType(c, &operand->type);

        if (operand->type.kind == TYPE_UNIT) {
            return LLVMConstNull(n->type.llvm);
        }

        return LLVMSizeOf(typeInMemory(operand->type));
    }

    case NODE_FN:
        compileFn(c, n);
        return n->as.fn.llvm;

    case NODE_VAR:
        // Temporary values
        compileStmt(c, n);
        return n->as.var.llvm;

    case NODE_STRUCT: {
        assert(n->as.structt.literalType);

        Node *temp = n->as.structt.literalTemp;
        assert(temp);

        LLVMValueRef tempValue = compileExpr(c, temp, false);
        for (Node *it = n->as.structt.fields.head; it; it = it->next) {
            assert(it->kind == NODE_BINARY);
            assert(it->as.binary.lhs->kind == NODE_FIELD);

            const size_t       index = it->as.binary.lhs->as.field.index;
            const LLVMValueRef fieldValue =
                LLVMBuildStructGEP2(c->builder, temp->type.llvm, tempValue, index, "");

            const LLVMValueRef assign = compileExpr(c, it->as.binary.rhs, false);
            LLVMBuildStore(c->builder, assign, fieldValue);
        }

        if (ref) {
            return tempValue;
        }

        return LLVMBuildLoad2(c->builder, typeInMemory(n->type), tempValue, "");
    };

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 21, "");
static void compileStmt(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_BLOCK:
        for (Node *it = n->as.block.head; it; it = it->next) {
            compileStmt(c, it);
        }
        break;

    case NODE_IF: {
        const LLVMValueRef cond = compileExpr(c, n->as.iff.condition, false);

        LLVMBasicBlockRef thenBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        LLVMBasicBlockRef elseBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");

        LLVMBasicBlockRef finalBlock = elseBlock;
        if (n->as.iff.antecedence) {
            finalBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        }

        LLVMBuildCondBr(c->builder, cond, thenBlock, elseBlock);

        // Then
        LLVMPositionBuilderAtEnd(c->builder, thenBlock);
        compileStmt(c, n->as.iff.consequence);
        LLVMBuildBr(c->builder, finalBlock);

        // Else
        if (n->as.iff.antecedence) {
            LLVMPositionBuilderAtEnd(c->builder, elseBlock);
            compileStmt(c, n->as.iff.antecedence);
            LLVMBuildBr(c->builder, finalBlock);
        }

        // Finally
        LLVMPositionBuilderAtEnd(c->builder, finalBlock);
    } break;

    case NODE_FOR: {
        if (n->as.forr.init) {
            compileStmt(c, n->as.forr.init);
        }

        LLVMBasicBlockRef condBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        LLVMBasicBlockRef bodyBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        LLVMBasicBlockRef finalBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");

        LLVMBasicBlockRef updateBlock = NULL;
        if (n->as.forr.update) {
            updateBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        }

        LLVMBuildBr(c->builder, condBlock);

        // Condition
        LLVMPositionBuilderAtEnd(c->builder, condBlock);
        if (n->as.forr.condition) {
            const LLVMValueRef condValue = compileExpr(c, n->as.forr.condition, false);
            LLVMBuildCondBr(c->builder, condValue, bodyBlock, finalBlock);
        } else {
            LLVMBuildBr(c->builder, bodyBlock);
        }

        // Body
        LLVMPositionBuilderAtEnd(c->builder, bodyBlock);
        compileStmt(c, n->as.forr.body);

        // Update
        if (n->as.forr.update) {
            LLVMBuildBr(c->builder, updateBlock);
            LLVMPositionBuilderAtEnd(c->builder, updateBlock);
            compileExpr(c, n->as.forr.update, false);
        }

        LLVMBuildBr(c->builder, condBlock);

        // Finally
        LLVMPositionBuilderAtEnd(c->builder, finalBlock);
    } break;

    case NODE_FLOW: {
        Node *operand = n->as.flow.operand;

        static_assert(COUNT_TOKENS == 47, "");
        switch (n->token.kind) {
        case TOKEN_RETURN: {
            if (operand) {
                LLVMBuildRet(c->builder, compileExpr(c, operand, false));
            } else {
                LLVMBuildRetVoid(c->builder);
            }

            LLVMBasicBlockRef deadBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
            LLVMPositionBuilderAtEnd(c->builder, deadBlock);
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_FN:
        compileFn(c, n);
        break;

    case NODE_VAR: {
        if (!n->type.llvm) {
            compileType(c, &n->type);
        }
        const LLVMTypeRef llvmType = typeInMemory(n->type);

        if (n->as.var.isExtern) {
            n->as.var.llvm = LLVMAddGlobal(c->module, llvmType, tempStrToCstr(n->token.str));
            LLVMSetLinkage(n->as.var.llvm, LLVMExternalLinkage);
            return;
        }

        if (n->as.var.local) {
            LLVMValueRef assign = NULL;
            if (n->as.var.expr) {
                assign = compileExpr(c, n->as.var.expr, false);
            } else {
                assign = LLVMConstNull(llvmType);
            }
            LLVMBuildStore(c->builder, assign, n->as.var.llvm);
        } else {
            n->as.var.llvm = LLVMAddGlobal(c->module, llvmType, ""); // TODO: Public variables
            LLVMSetInitializer(n->as.var.llvm, LLVMConstNull(llvmType));
        }
    } break;

    case NODE_TYPE:
        static_assert(COUNT_TYPES == 17, ""); // Pass
        break;

    case NODE_EXTERN:
        for (Node *it = n->as.externn.definitions.head; it; it = it->next) {
            compileStmt(c, it);
        }
        break;

    case NODE_PRINT: {
        LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
        LLVMValueRef indices[] = {zero, zero};
        LLVMValueRef operandValue = compileExpr(c, n->as.print.operand, false);

        LLVMValueRef fmtPtr = NULL;
        if (typeIsSigned(n->as.print.operand->type)) {
            fmtPtr = LLVMBuildInBoundsGEP2(
                c->builder, c->printSFmtType, c->printSFmtValue, indices, len(indices), "");
        } else {
            fmtPtr = LLVMBuildInBoundsGEP2(
                c->builder, c->printUFmtType, c->printUFmtValue, indices, len(indices), "");
        }

        LLVMValueRef args[] = {
            fmtPtr,
            operandValue,
        };

        LLVMBuildCall2(c->builder, c->printFuncType, c->printFuncValue, args, len(args), "");
    } break;

    default:
        compileExpr(c, n, false);
        break;
    }
}

static void compileFn(Compiler *c, Node *n) {
    compileType(c, &n->type);

    if (n->as.fn.body) {
        n->as.fn.llvm = LLVMAddFunction(c->module, "", n->type.llvm); // TODO: Public functions
    } else {
        assert(n->token.kind == TOKEN_IDENT);
        n->as.fn.llvm = LLVMAddFunction(c->module, tempStrToCstr(n->token.str), n->type.llvm);
        return;
    }

    const FnCompiler fnCompilerSave = fnCompilerBegin(c, n);
    {
        LLVMBasicBlockRef body = LLVMAppendBasicBlock(n->as.fn.llvm, "entry");
        LLVMPositionBuilderAtEnd(c->builder, body);

        for (Node *it = n->as.fn.args.head; it; it = it->next) {
            NodeArg *arg = &it->as.arg;
            if (arg->memory) {
                LLVMValueRef argLLVM = LLVMGetParam(c->fnCompiler.fn, arg->index);
                arg->llvm = LLVMBuildAlloca(c->builder, typeInMemory(it->type), "");
                LLVMBuildStore(c->builder, argLLVM, arg->llvm);
            }
        }

        for (size_t i = 0; i < n->as.fn.locals.length; i++) {
            Node *it = n->as.fn.locals.data[i];
            if (it->kind == NODE_VAR) {
                compileType(c, &it->type);
                it->as.var.llvm = LLVMBuildAlloca(c->builder, typeInMemory(it->type), "");
            }
        }

        compileStmt(c, n->as.fn.body);

        if (n->as.fn.ret) {
            LLVMBuildRet(c->builder, LLVMConstNull(typeInMemory(n->as.fn.ret->type)));
        } else {
            LLVMBuildRetVoid(c->builder);
        }
    }
    fnCompilerEnd(c, fnCompilerSave);
}

static Node *getMain(Context context) {
    Node *main = scopeFind(context.globals, strFromCstr("main"), false);
    if (!main) {
        fprintf(stderr, "ERROR: Function 'main' is not defined\n");
        exit(1);
    }

    if (main->kind != NODE_FN) {
        fprintf(
            stderr,
            PosFmt "ERROR: Function 'main' must be a function literal\n",
            PosArg(main->token.pos));

        exit(1);
    }

    NodeFn mainFn = main->as.fn;
    if (mainFn.arity) {
        fprintf(
            stderr,
            PosFmt "ERROR: Function 'main' cannot take any arguments\n",
            PosArg(main->token.pos));

        exit(1);
    }

    if (mainFn.ret) {
        fprintf(
            stderr,
            PosFmt "ERROR: Function 'main' cannot return anything\n",
            PosArg(main->token.pos));

        exit(1);
    }
    return main;
}

static_assert(COUNT_NODES == 21, "");
static void preCompile(Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM:
        n->type = typeResolve(n->type);
        break;

    case NODE_CALL:
        n->type = typeResolve(n->type);

        preCompile(n->as.call.fn);
        for (Node *it = n->as.call.args.head; it; it = it->next) {
            preCompile(it);
        }
        break;

    case NODE_CAST:
        n->type = typeResolve(n->type);
        preCompile(n->as.cast.from);
        break;

    case NODE_UNARY:
        n->type = typeResolve(n->type);
        preCompile(n->as.unary.operand);
        break;

    case NODE_ARRAY:
        n->type = typeResolve(n->type);
        preCompile(n->as.array.base);
        preCompile(n->as.array.length);

        for (Node *it = n->as.array.literalInits.head; it; it = it->next) {
            preCompile(it);
        }
        break;

    case NODE_INDEX:
        n->type = typeResolve(n->type);
        preCompile(n->as.index.base);
        preCompile(n->as.index.at);
        preCompile(n->as.index.end);
        break;

    case NODE_BINARY:
        n->type = typeResolve(n->type);
        preCompile(n->as.binary.lhs);
        preCompile(n->as.binary.rhs);
        break;

    case NODE_MEMBER:
        n->type = typeResolve(n->type);
        preCompile(n->as.member.lhs);
        break;

    case NODE_SIZEOF:
        n->as.sizeoff.operand->type = typeResolve(n->as.sizeoff.operand->type);
        break;

    case NODE_BLOCK:
        for (Node *it = n->as.block.head; it; it = it->next) {
            preCompile(it);
        }
        break;

    case NODE_IF:
        preCompile(n->as.iff.condition);
        preCompile(n->as.iff.consequence);
        preCompile(n->as.iff.antecedence);
        break;

    case NODE_FOR:
        preCompile(n->as.forr.init);
        preCompile(n->as.forr.condition);
        preCompile(n->as.forr.update);
        preCompile(n->as.forr.body);
        break;

    case NODE_FLOW:
        static_assert(COUNT_TOKENS == 47, "");
        switch (n->token.kind) {
        case TOKEN_RETURN:
            preCompile(n->as.flow.operand);
            break;

        default:
            unreachable();
        }
        break;

    case NODE_FN:
        for (Node *it = n->as.fn.args.head; it; it = it->next) {
            preCompile(it);
        }
        preCompile(n->as.fn.ret);

        for (size_t i = 0; i < n->as.fn.locals.length; i++) {
            preCompile(n->as.fn.locals.data[i]);
        }
        preCompile(n->as.fn.body);
        break;

    case NODE_ARG:
        n->type = typeResolve(n->type);
        break;

    case NODE_VAR:
        n->type = typeResolve(n->type);
        preCompile(n->as.var.expr);
        preCompile(n->as.var.type);
        break;

    case NODE_TYPE:
        preCompile(n->as.type.definition);
        break;

    case NODE_FIELD:
        n->type = typeResolve(n->type);
        preCompile(n->as.field.type);
        break;

    case NODE_STRUCT: {
        n->type = typeResolve(n->type);
        preCompile(n->as.structt.literalType);

        for (Node *it = n->as.structt.fields.head; it; it = it->next) {
            preCompile(it);
        }
        preCompile(n->as.structt.literalTemp);
    } break;

    case NODE_EXTERN:
        for (Node *it = n->as.externn.definitions.head; it; it = it->next) {
            preCompile(it);
        }
        break;

    case NODE_PRINT:
        preCompile(n->as.print.operand);
        break;

    default:
        unreachable();
    }
}

void compileProgram(Context context, const char *executableName) {
    Node *mainFn = getMain(context);

    Compiler c = {0};
    c.context = context;
    c.module = LLVMModuleCreateWithName(executableName);
    c.builder = LLVMCreateBuilder();

    // The 'print' keyword
    {
        const char printSFmtStr[] = "%ld\n";
        c.printSFmtType = LLVMArrayType(LLVMInt8Type(), len(printSFmtStr));

        LLVMValueRef printSFmtConst = LLVMConstString(printSFmtStr, strlen(printSFmtStr), false);
        c.printSFmtValue = LLVMAddGlobal(c.module, c.printSFmtType, "");

        LLVMSetInitializer(c.printSFmtValue, printSFmtConst);
        LLVMSetGlobalConstant(c.printSFmtValue, true);
        LLVMSetLinkage(c.printSFmtValue, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(c.printSFmtValue, LLVMGlobalUnnamedAddr);

        const char printUFmtStr[] = "%lu\n";
        c.printUFmtType = LLVMArrayType(LLVMInt8Type(), len(printUFmtStr));

        LLVMValueRef printUFmtConst = LLVMConstString(printUFmtStr, strlen(printUFmtStr), false);
        c.printUFmtValue = LLVMAddGlobal(c.module, c.printUFmtType, "");

        LLVMSetInitializer(c.printUFmtValue, printUFmtConst);
        LLVMSetGlobalConstant(c.printUFmtValue, true);
        LLVMSetLinkage(c.printUFmtValue, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(c.printUFmtValue, LLVMGlobalUnnamedAddr);

        LLVMTypeRef printfArgs[] = {LLVMPointerType(LLVMInt8Type(), 0)};
        c.printFuncType = LLVMFunctionType(LLVMInt32Type(), printfArgs, len(printfArgs), true);
        c.printFuncValue = LLVMAddFunction(c.module, "printf", c.printFuncType);
    }

    // The slice type
    {
        LLVMTypeRef sliceFields[] = {
            LLVMPointerType(LLVMVoidType(), 0),
            LLVMInt64Type(),
        };
        c.sliceType = LLVMStructType(sliceFields, len(sliceFields), false);
    }

    // Pre Compile
    {
        for (size_t i = 0; i < context.globals.length; i++) {
            preCompile(context.globals.data[i]);
        }

        for (size_t i = 0; i < context.globalTemps.length; i++) {
            preCompile(context.globalTemps.data[i]);
        }
    }

    // The globals
    {
        for (size_t i = 0; i < context.globals.length; i++) {
            compileStmt(&c, context.globals.data[i]);
        }
    }

    // The main function
    {
        LLVMTypeRef  cMainType = LLVMFunctionType(LLVMInt32Type(), NULL, 0, false);
        LLVMValueRef cMainFunc = LLVMAddFunction(c.module, "main", cMainType);

        LLVMBasicBlockRef cMainEntry = LLVMAppendBasicBlock(cMainFunc, "entry");
        LLVMPositionBuilderAtEnd(c.builder, cMainEntry);

        {
            for (size_t i = 0; i < context.globalTemps.length; i++) {
                Node *it = context.globalTemps.data[i];
                assert(it->kind == NODE_VAR);

                compileType(&c, &it->type);
                it->as.var.llvm = LLVMBuildAlloca(c.builder, typeInMemory(it->type), "");
            }
        }

        // Initialize the global variables
        for (size_t i = 0; i < context.globals.length; i++) {
            Node *it = context.globals.data[i];
            if (it->kind == NODE_VAR) {
                Node *expr = it->as.var.expr;
                if (expr) {
                    LLVMValueRef init = compileExpr(&c, expr, false);
                    LLVMValueRef itLLVM = definitionLLVMValue(it);
                    LLVMBuildStore(c.builder, init, itLLVM);
                }
            }
        }

        LLVMBuildCall2(c.builder, mainFn->type.llvm, mainFn->as.fn.llvm, NULL, 0, "");
        LLVMBuildRet(c.builder, LLVMConstInt(LLVMInt32Type(), 0, false));
    }

    // Verify LLVM Module
    {
        if (LLVMVerifyModule(c.module, LLVMAbortProcessAction, NULL)) {
            fprintf(stderr, "ERROR: Invalid LLVM IR\n");
            exit(1);
        }
    }

    // Object File Generation
    const char *objectFileName = tempSprintf("%s.o", executableName);
    {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();

        char         *triple = LLVMGetDefaultTargetTriple();
        LLVMTargetRef target = NULL;
        if (LLVMGetTargetFromTriple(triple, &target, NULL)) {
            fprintf(stderr, "ERROR: Could not get machine target\n");
            exit(1);
        }

        LLVMTargetMachineRef targetMachine = LLVMCreateTargetMachine(
            target, triple, "", "", LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);

        if (LLVMTargetMachineEmitToFile(
                targetMachine, c.module, objectFileName, LLVMObjectFile, NULL)) {
            fprintf(stderr, "ERROR: Could not create object file\n");
            exit(1);
        }

        LLVMDisposeBuilder(c.builder);
        LLVMDisposeModule(c.module);
        LLVMDisposeTargetMachine(targetMachine);
    }

    // Linking
    {
        const char *args[] = {
            "cc",
            "-o",
            executableName,
            objectFileName,
            NULL,
        };

        if (runCommand(args)) {
            fprintf(stderr, "ERROR: Could not link executable\n");
            exit(1);
        }

        removeFile(objectFileName);
    }
}
