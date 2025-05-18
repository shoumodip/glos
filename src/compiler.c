#include "compiler.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

// NOTE: This is temporary
#include <llvm-c/Analysis.h>
#include <stdbool.h>

typedef struct {
    LLVMValueRef      fn;
    LLVMBasicBlockRef block;
} FnCompiler;

typedef struct {
    Context    context;
    FnCompiler fnCompiler;

    LLVMModuleRef  module;
    LLVMBuilderRef builder;

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
        return LLVMPointerType(type.llvm, 0);
    }

    return type.llvm;
}

static_assert(COUNT_TYPES == 12, "");
static void compileType(Node *n) {
    if (typeIsPointer(n->type)) {
        n->type.llvm = LLVMPointerType(LLVMVoidType(), 0);
        return;
    }

    switch (n->type.kind) {
    case TYPE_UNIT:
        n->type.llvm = LLVMVoidType();
        break;

    case TYPE_BOOL:
        n->type.llvm = LLVMInt1Type();
        break;

    case TYPE_I8:
    case TYPE_U8:
        n->type.llvm = LLVMInt8Type();
        break;

    case TYPE_I16:
    case TYPE_U16:
        n->type.llvm = LLVMInt16Type();
        break;

    case TYPE_I32:
    case TYPE_U32:
        n->type.llvm = LLVMInt32Type();
        break;

    case TYPE_I64:
    case TYPE_U64:
        n->type.llvm = LLVMInt64Type();
        break;

    case TYPE_FN: {
        assert(n->type.spec);
        NodeFn *fn = &n->type.spec->as.fn;

        LLVMTypeRef *argsLLVM = calloc(fn->arity, sizeof(LLVMTypeRef));
        for (Node *it = fn->args.head; it; it = it->next) {
            compileType(it);
            argsLLVM[it->as.arg.index] = typeInMemory(it->type);
        }

        LLVMTypeRef returnType = NULL;
        if (fn->ret) {
            compileType(fn->ret);
            returnType = typeInMemory(fn->ret->type);
        } else {
            returnType = LLVMVoidType();
        }

        n->type.llvm = LLVMFunctionType(returnType, argsLLVM, fn->arity, false);
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 14, "");
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

static_assert(COUNT_NODES == 14, "");
static LLVMValueRef compileExpr(Compiler *c, Node *n, bool ref) {
    compileType(n);

    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 36, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, true);

        case TOKEN_BOOL:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, false);

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

        return LLVMBuildCall2(c->builder, fn->type.llvm, fnValue, argsLLVM, call.arity, "");
    };

    case NODE_CAST: {
        Node              *from = n->as.cast.from;
        const LLVMValueRef fromValue = compileExpr(c, from, false);

        const Type fromType = from->type;
        const Type toType = n->type;
        if (typeEq(fromType, toType)) {
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

        static_assert(COUNT_TYPES == 12, "");
        const size_t intSizes[COUNT_TYPES] = {
            [TYPE_I8] = 8,
            [TYPE_I16] = 16,
            [TYPE_I32] = 32,
            [TYPE_I64] = 64,

            [TYPE_U8] = 8,
            [TYPE_U16] = 16,
            [TYPE_U32] = 32,
            [TYPE_U64] = 64,
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

        static_assert(COUNT_TOKENS == 36, "");
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

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 36, "");
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

    case NODE_SIZEOF: {
        Node *operand = n->as.sizeoff.operand;
        compileType(operand);

        if (operand->type.kind == TYPE_UNIT) {
            return LLVMConstNull(n->type.llvm);
        }

        return LLVMSizeOf(typeInMemory(operand->type));
    }

    case NODE_FN:
        compileFn(c, n);
        return n->as.fn.llvm;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 14, "");
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
        assert(!n->as.forr.init);
        assert(!n->as.forr.update);

        LLVMBasicBlockRef condBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        LLVMBasicBlockRef bodyBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");
        LLVMBasicBlockRef finalBlock = LLVMAppendBasicBlock(c->fnCompiler.fn, "");

        LLVMBuildBr(c->builder, condBlock);

        // Condition
        LLVMPositionBuilderAtEnd(c->builder, condBlock);
        const LLVMValueRef condValue = compileExpr(c, n->as.forr.condition, false);
        LLVMBuildCondBr(c->builder, condValue, bodyBlock, finalBlock);

        // Body
        LLVMPositionBuilderAtEnd(c->builder, bodyBlock);
        compileStmt(c, n->as.forr.body);
        LLVMBuildBr(c->builder, condBlock);

        // Finally
        LLVMPositionBuilderAtEnd(c->builder, finalBlock);
    } break;

    case NODE_FLOW: {
        Node *operand = n->as.flow.operand;

        static_assert(COUNT_TOKENS == 36, "");
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
        if (n->as.var.local) {
            // The type was already compiled in the function prelude
            const LLVMTypeRef llvmType = typeInMemory(n->type);

            LLVMValueRef assign = NULL;
            if (n->as.var.expr) {
                assign = compileExpr(c, n->as.var.expr, false);
            } else {
                assign = LLVMConstNull(llvmType);
            }
            LLVMBuildStore(c->builder, assign, n->as.var.llvm);
        } else {
            compileType(n);
            const LLVMTypeRef llvmType = typeInMemory(n->type);

            n->as.var.llvm = LLVMAddGlobal(c->module, llvmType, ""); // TODO: Public variables
            LLVMSetInitializer(n->as.var.llvm, LLVMConstNull(llvmType));
        }
    } break;

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
    compileType(n);
    n->as.fn.llvm = LLVMAddFunction(c->module, "", n->type.llvm); // TODO: Public functions

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
                compileType(it);
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
    Node *main = scopeFind(context.globals, strFromCstr("main"));
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
