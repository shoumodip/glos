#include "compiler.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

// NOTE: This is temporary
#include <llvm-c/Analysis.h>

typedef struct {
    LLVMModuleRef  module;
    LLVMBuilderRef builder;

    // The 'print' keyword
    LLVMTypeRef  printFmtType;
    LLVMValueRef printFmtValue;
    LLVMTypeRef  printFuncType;
    LLVMValueRef printFuncValue;
} Compiler;

static_assert(COUNT_NODES == 4, "");
static_assert(COUNT_TOKENS == 9, "");
static LLVMValueRef compileExpr(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_ATOM:
        switch (n->token.kind) {
        case TOKEN_INT:
            return LLVMConstInt(LLVMInt64Type(), n->token.as.integer, true);

        case TOKEN_BOOL:
            return LLVMConstInt(LLVMInt1Type(), n->token.as.integer, false);

        default:
            unreachable();
        }
        break;

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        switch (n->token.kind) {
        case TOKEN_SUB: {
            const LLVMValueRef operandValue = compileExpr(c, operand);
            return LLVMBuildNeg(c->builder, operandValue, "");
        };

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        switch (n->token.kind) {
        case TOKEN_ADD: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            return LLVMBuildAdd(c->builder, lhsValue, rhsValue, "");
        }

        case TOKEN_SUB: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            return LLVMBuildSub(c->builder, lhsValue, rhsValue, "");
        }

        case TOKEN_MUL: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            return LLVMBuildMul(c->builder, lhsValue, rhsValue, "");
        }

        case TOKEN_DIV: {
            const LLVMValueRef lhsValue = compileExpr(c, lhs);
            const LLVMValueRef rhsValue = compileExpr(c, rhs);
            return LLVMBuildSDiv(c->builder, lhsValue, rhsValue, "");
        }

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 4, "");
static_assert(COUNT_TOKENS == 9, "");
static void compileStmt(Compiler *c, Node *n) {
    switch (n->kind) {
    case NODE_PRINT: {
        LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
        LLVMValueRef indices[] = {zero, zero};
        LLVMValueRef fmtPtr = LLVMBuildInBoundsGEP2(
            c->builder, c->printFmtType, c->printFmtValue, indices, len(indices), "");

        LLVMValueRef args[] = {
            fmtPtr,
            compileExpr(c, n->as.print.operand),
        };

        LLVMBuildCall2(c->builder, c->printFuncType, c->printFuncValue, args, len(args), "");
    } break;

    default:
        compileExpr(c, n);
        break;
    }
}

void compileProgram(Nodes nodes, const char *executableName) {
    Compiler c = {0};
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
        LLVMSetFunctionCallConv(c.printFuncValue, LLVMCCallConv);
    }

    // The main program
    {
        LLVMTypeRef  mainType = LLVMFunctionType(LLVMInt32Type(), NULL, 0, false);
        LLVMValueRef mainFunc = LLVMAddFunction(c.module, "main", mainType);

        LLVMBasicBlockRef mainEntry = LLVMAppendBasicBlock(mainFunc, "entry");
        LLVMPositionBuilderAtEnd(c.builder, mainEntry);

        for (Node *it = nodes.head; it; it = it->next) {
            compileStmt(&c, it);
        }

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
