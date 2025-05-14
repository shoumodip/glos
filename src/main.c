#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "basic.h"

int main(void) {
    LLVMModuleRef module = LLVMModuleCreateWithName("");

    // Debug Print
    const char   dpf_str[] = "%ld\n";
    LLVMTypeRef  dpf_type = LLVMArrayType(LLVMInt8Type(), len(dpf_str));
    LLVMValueRef dpf_const = LLVMConstString(dpf_str, len(dpf_str) - 1, false);
    LLVMValueRef dpf_global = LLVMAddGlobal(module, dpf_type, "");

    LLVMSetInitializer(dpf_global, dpf_const);
    LLVMSetGlobalConstant(dpf_global, true);
    LLVMSetLinkage(dpf_global, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(dpf_global, LLVMGlobalUnnamedAddr);

    LLVMTypeRef printf_args[] = {LLVMPointerType(LLVMInt8Type(), 0)};
    LLVMTypeRef printf_type =
        LLVMFunctionType(LLVMInt32Type(), printf_args, len(printf_args), true);

    LLVMValueRef printf_func = LLVMAddFunction(module, "printf", printf_type);
    LLVMSetFunctionCallConv(printf_func, LLVMCCallConv);

    // Main
    LLVMTypeRef  main_type = LLVMFunctionType(LLVMInt32Type(), NULL, 0, false);
    LLVMValueRef main_func = LLVMAddFunction(module, "main", main_type);

    LLVMBuilderRef    builder = LLVMCreateBuilder();
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(main_func, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);

    LLVMValueRef zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
    LLVMValueRef indices[] = {zero, zero};
    LLVMValueRef dpf_ptr =
        LLVMBuildInBoundsGEP2(builder, dpf_type, dpf_global, indices, len(indices), "");

    LLVMValueRef call_args[] = {dpf_ptr, LLVMConstInt(LLVMInt64Type(), 69, true)};
    LLVMBuildCall2(builder, printf_type, printf_func, call_args, len(call_args), "");

    // Main Return
    LLVMBuildRet(builder, LLVMConstInt(LLVMInt32Type(), 0, false));

    // Compilation
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    char *error = NULL;
    char *triple = LLVMGetDefaultTargetTriple();

    LLVMTargetRef target = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        fprintf(stderr, "ERROR: Could not get LLVM target\n");
        exit(1);
    }

    LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(
        target, triple, "", "", LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);

    LLVMTargetMachineEmitToFile(target_machine, module, "hello.o", LLVMObjectFile, &error);
    if (error) {
        fprintf(stderr, "ERROR: Could not emit LLVM object file\n");
        exit(1);
    }

    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMDisposeTargetMachine(target_machine);

    // Linking
    const char *args[] = {
        "cc",
        "-o",
        "hello",
        "hello.o",
        NULL,
    };

    if (!runCommand(args)) {
        fprintf(stderr, "ERROR: Could not link executable\n");
        exit(1);
    }

    removeFile("hello.o");
    return 0;
}
