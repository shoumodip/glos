#ifndef COMPILER_H
#define COMPILER_H

#include "context.h"
#include "parser.h"

#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef DA(const char *) Link_Flags;

void link_flags_add_libpath(Link_Flags *ls, SV path);
void link_flags_add_libname(Link_Flags *ls, SV name);

typedef struct {
    size_t       id;
    LLVMValueRef info;
} Type_Info;

typedef struct {
    uintptr_t uid;
    SV        name;
} Method_Spec;

typedef struct {
    // These are used only by the analyzer
    Type main_fn_type;
    DA(Type_Struct_Field) struct_fields;

    Type interpolated_string_type;

    HT(Method_Spec, Node_Fn *) methods_table;
    DA(Node_Fn *) methods_list;

    Type comparison_type;

    // These are used both by the analyzer and the compiler
    Context context;

    Parser  *parser;
    Modules *modules;
    Node_Fn *main_fn;

    Module *main_module;
    Module *builtin_module;

    Type type_info_type;         // This holds `Type_Info`
    Type type_info_pointer_type; // This holds `&Type_Info`

    size_t            type_info_variants[COUNT_TYPES];
    const Type_Union *type_info_variants_union;

    Type source_code_location_type;
    Type any_type;

    // Rest all are only used by compiler
    Cmd        *cmd;
    Link_Flags *link_flags;

    DA(Node *) defers;
    size_t defers_start;
    size_t loop_defers_start;

    DA(LLVMValueRef) arg_values;
    DA(LLVMValueRef) group_values;

    LLVMContextRef       llvm_context;
    LLVMModuleRef        llvm_module;
    LLVMTargetDataRef    llvm_target_data;
    LLVMTargetMachineRef llvm_target_machine;

    LLVMBuilderRef llvm_builder;
    LLVMValueRef   llvm_fn;
    LLVMValueRef   llvm_fn_last_alloca;

    unsigned int llvm_attribute_sret;
    unsigned int llvm_attribute_byval;
    unsigned int llvm_attribute_alwaysinline;

    LLVMBasicBlockRef llvm_loop_break;
    LLVMBasicBlockRef llvm_loop_continue;

    LLVMDIBuilderRef llvm_debug_builder;
    LLVMMetadataRef  llvm_debug_compile_unit;
    LLVMMetadataRef  llvm_debug_scope;

    HT(const char *, LLVMMetadataRef) llvm_debug_files;

    size_t type_id_iota;
    HT(Type, Type_Info) type_info_cache;

    // Slice :: struct {
    //     data: rawptr
    //     count: i64
    // }
    LLVMTypeRef llvm_slice_type;

    // Trait :: struct {
    //     type: Type
    //     data: rawptr
    //     impl: &[N]Trait_Method
    // }
    LLVMTypeRef llvm_trait_type;

    // TODO: Cache the debug for these types as well

    size_t iota_anonymous_fn;
} Compiler;

size_t compile_sizeof(Compiler *c, Type *type);
void   compiler_build(Compiler *c, const char *output_path);

#endif // COMPILER_H
