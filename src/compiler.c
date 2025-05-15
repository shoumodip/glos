#include "compiler.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

// NOTE: This is temporary
#include <llvm-c/Analysis.h>

typedef struct {
    LLVMValueRef   fn;
    LLVMBuilderRef builder;
} Emitter;

typedef struct {
    Context context;
    Emitter emitter;

    LLVMModuleRef module;

    // The 'print' keyword
    LLVMTypeRef  printFmtType;
    LLVMValueRef printFmtValue;
    LLVMTypeRef  printFuncType;
    LLVMValueRef printFuncValue;
} Compiler;

static_assert(COUNT_NODES == 7, "");
static LLVMValueRef compileExpr(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 16, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type.llvm = LLVMInt64Type();
            return LLVMConstInt(n->type.llvm, n->token.as.integer, true);

        case TOKEN_BOOL:
            n->type.llvm = LLVMInt1Type();
            return LLVMConstInt(n->type.llvm, n->token.as.integer, false);

        default:
            unreachable();
        }
        break;

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        static_assert(COUNT_TOKENS == 16, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            const LLVMValueRef operandValue = compileExpr(c, operand);
            n->type.llvm = operand->type.llvm;
            return LLVMBuildNeg(c->emitter.builder, operandValue, "");
        };

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 16, "");
        switch (n->token.kind) {
        case TOKEN_ADD: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            n->type.llvm = lhs->type.llvm;
            return LLVMBuildAdd(c->emitter.builder, lhsValue, rhsValue, "");
        }

        case TOKEN_SUB: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            n->type.llvm = lhs->type.llvm;
            return LLVMBuildSub(c->emitter.builder, lhsValue, rhsValue, "");
        }

        case TOKEN_MUL: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            n->type.llvm = lhs->type.llvm;
            return LLVMBuildMul(c->emitter.builder, lhsValue, rhsValue, "");
        }

        case TOKEN_DIV: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            n->type.llvm = lhs->type.llvm;
            return LLVMBuildSDiv(c->emitter.builder, lhsValue, rhsValue, "");
        }

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 7, "");
static_assert(COUNT_TOKENS == 16, "");
static void compileStmt(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_BLOCK:
        for (Node *it = n->as.block.head; it; it = it->next) {
            compileStmt(c, it);
        }
        break;

    case NODE_IF: {
        const LLVMValueRef cond = compileExpr(c, n->as.iff.condition);

        LLVMBasicBlockRef thenBlock = LLVMAppendBasicBlock(c->emitter.fn, "");
        LLVMBasicBlockRef elseBlock = LLVMAppendBasicBlock(c->emitter.fn, "");

        LLVMBasicBlockRef finalBlock = elseBlock;
        if (n->as.iff.antecedence) {
            finalBlock = LLVMAppendBasicBlock(c->emitter.fn, "");
        }

        LLVMBuildCondBr(c->emitter.builder, cond, thenBlock, elseBlock);

        // Then
        LLVMPositionBuilderAtEnd(c->emitter.builder, thenBlock);
        compileStmt(c, n->as.iff.consequence);
        LLVMBuildBr(c->emitter.builder, finalBlock);

        // Else
        if (n->as.iff.antecedence) {
            LLVMPositionBuilderAtEnd(c->emitter.builder, elseBlock);
            compileStmt(c, n->as.iff.antecedence);
            LLVMBuildBr(c->emitter.builder, finalBlock);
        }

        // Finally
        LLVMPositionBuilderAtEnd(c->emitter.builder, finalBlock);
    } break;

    case NODE_FN: {
        assert(!n->as.fn.args);
        assert(!n->as.fn.ret);

        n->type.llvm = LLVMFunctionType(LLVMVoidType(), NULL, 0, false);
        n->llvm = LLVMAddFunction(c->module, "", n->type.llvm); // TODO: Public functions

        const Emitter emitterSave = c->emitter;
        c->emitter.fn = n->llvm;
        {
            LLVMBasicBlockRef body = LLVMAppendBasicBlock(n->llvm, "entry");
            LLVMPositionBuilderAtEnd(c->emitter.builder, body);
            compileStmt(c, n->as.fn.body);
            LLVMBuildRetVoid(c->emitter.builder);
        }
        c->emitter = emitterSave;
    } break;

    case NODE_PRINT: {
        LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
        LLVMValueRef indices[] = {zero, zero};
        LLVMValueRef fmtPtr = LLVMBuildInBoundsGEP2(
            c->emitter.builder, c->printFmtType, c->printFmtValue, indices, len(indices), "");

        LLVMValueRef args[] = {
            fmtPtr,
            compileExpr(c, n->as.print.operand),
        };

        LLVMBuildCall2(
            c->emitter.builder, c->printFuncType, c->printFuncValue, args, len(args), "");
    } break;

    default:
        compileExpr(c, n);
        break;
    }
}

static Node *getMain(Context context) {
    Node *main = scopeFind(context.globals, strFromCstr("main"));
    if (!main) {
        fprintf(stderr, "ERROR: Function 'main' is not defined\n");
        exit(1);
    }

    const Type expected = {.kind = TYPE_FN};
    if (!typeEq(main->type, expected)) {
        fprintf(
            stderr,
            "ERROR: Expected function 'main' to be of type '%s', got '%s'\n",
            typeToString(expected),
            typeToString(main->type));

        exit(1);
    }

    return main;
}

void compileProgram(Context context, const char *executableName) {
    Node *mainFn = getMain(context);

    Compiler c = {0};
    c.context = context;
    c.module = LLVMModuleCreateWithName(executableName);
    c.emitter.builder = LLVMCreateBuilder();

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
        LLVMPositionBuilderAtEnd(c.emitter.builder, cMainEntry);
        LLVMBuildCall2(c.emitter.builder, mainFn->type.llvm, mainFn->llvm, NULL, 0, "");
        LLVMBuildRet(c.emitter.builder, LLVMConstInt(LLVMInt32Type(), 0, false));
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

        LLVMDisposeBuilder(c.emitter.builder);
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
