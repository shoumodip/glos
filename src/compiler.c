#include "compiler.h"
#include "ast.h"

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
    LLVMTypeRef  printFmtType;
    LLVMValueRef printFmtValue;
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

static_assert(COUNT_TYPES == 4, "");
static void compileType(Node *n) {
    if (n->type.ref) {
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

    case TYPE_I64:
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

static_assert(COUNT_NODES == 12, "");
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
    if (type.ref == 0) {
        return;
    }

    LLVMTypeRef  typeTemp = LLVMInt64Type();
    LLVMValueRef lhsTemp = LLVMBuildPtrToInt(c->builder, *lhs, typeTemp, "");
    LLVMValueRef rhsTemp = LLVMBuildPtrToInt(c->builder, *rhs, typeTemp, "");

    *lhs = lhsTemp;
    *rhs = rhsTemp;
}

static LLVMValueRef afterArith(Compiler *c, Type type, LLVMValueRef result) {
    if (type.ref == 0) {
        return result;
    }

    return LLVMBuildIntToPtr(c->builder, result, type.llvm, "");
}

static void compileFn(Compiler *c, Node *n);

static_assert(COUNT_NODES == 12, "");
static LLVMValueRef compileExpr(Compiler *c, Node *n, bool ref) {
    compileType(n);

    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 37, "");
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

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        static_assert(COUNT_TOKENS == 37, "");
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

        static_assert(COUNT_TOKENS == 37, "");
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
            return afterArith(c, n->type, LLVMBuildSDiv(c->builder, lhsValue, rhsValue, ""));
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
            return afterArith(c, n->type, LLVMBuildAShr(c->builder, lhsValue, rhsValue, ""));
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
            return LLVMBuildICmp(c->builder, LLVMIntSGT, lhsValue, rhsValue, "");
        }

        case TOKEN_GE: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            return LLVMBuildICmp(c->builder, LLVMIntSGE, lhsValue, rhsValue, "");
        }

        case TOKEN_LT: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            return LLVMBuildICmp(c->builder, LLVMIntSLT, lhsValue, rhsValue, "");
        }

        case TOKEN_LE: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);
            const LLVMValueRef rhsValue = compileExpr(c, rhs, false);
            return LLVMBuildICmp(c->builder, LLVMIntSLE, lhsValue, rhsValue, "");
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

        case TOKEN_AS: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs, false);

            const Type from = lhs->type;
            const Type to = n->type;
            if (typeEq(from, to)) {
                return lhsValue;
            }

            const LLVMTypeRef toLLVM = typeInMemory(to);
            if (from.ref != 0) {
                if (to.ref != 0) {
                    // Pointer -> Pointer
                    return LLVMBuildBitCast(c->builder, lhsValue, toLLVM, "");
                } else {
                    // Pointer -> Integer
                    return LLVMBuildPtrToInt(c->builder, lhsValue, toLLVM, "");
                }
            }

            if (from.kind == TYPE_BOOL) {
                // Boolean -> Integer
                return LLVMBuildZExt(c->builder, lhsValue, toLLVM, "");
            }

            static_assert(COUNT_TYPES == 4, "");
            const size_t intSizes[COUNT_TYPES] = {
                [TYPE_I64] = 64,
            };

            if (intSizes[from.kind]) {
                if (to.ref != 0) {
                    // Integer -> Pointer
                    return LLVMBuildIntToPtr(c->builder, lhsValue, toLLVM, "");
                }

                if (to.kind == TYPE_BOOL) {
                    // Integer -> Boolean
                    const LLVMValueRef zero = LLVMConstNull(typeInMemory(from));
                    return LLVMBuildICmp(c->builder, LLVMIntNE, lhsValue, zero, "");
                }
            }

            unreachable();
        }

        default:
            unreachable();
        }
    } break;

    case NODE_FN:
        compileFn(c, n);
        return n->as.fn.llvm;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 12, "");
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

        static_assert(COUNT_TOKENS == 37, "");
        switch (n->token.kind) {
        case TOKEN_RETURN: {
            LLVMValueRef operandValue = NULL;
            if (operand) {
                operandValue = compileExpr(c, operand, false);
            }

            if (n->type.kind == TYPE_UNIT) {
                LLVMBuildRetVoid(c->builder);
            } else {
                LLVMBuildRet(c->builder, operandValue);
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
        LLVMValueRef fmtPtr = LLVMBuildInBoundsGEP2(
            c->builder, c->printFmtType, c->printFmtValue, indices, len(indices), "");

        LLVMValueRef args[] = {
            fmtPtr,
            compileExpr(c, n->as.print.operand, false),
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
        const char printFmtStr[] = "%ld\n";
        c.printFmtType = LLVMArrayType(LLVMInt8Type(), len(printFmtStr));

        LLVMValueRef printFmtConst = LLVMConstString(printFmtStr, strlen(printFmtStr), false);
        c.printFmtValue = LLVMAddGlobal(c.module, c.printFmtType, "");

        LLVMSetInitializer(c.printFmtValue, printFmtConst);
        LLVMSetGlobalConstant(c.printFmtValue, true);
        LLVMSetLinkage(c.printFmtValue, LLVMPrivateLinkage);
        LLVMSetUnnamedAddr(c.printFmtValue, LLVMGlobalUnnamedAddr);

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
