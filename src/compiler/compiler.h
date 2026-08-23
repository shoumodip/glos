#ifndef COMPILER_INTERNAL_H
#define COMPILER_INTERNAL_H

#include "../compiler.h"
#include "../contract.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

// Basics //////////////////////////////////////////////////////////////////////////////////////////
#define ABI_DIRECT_TYPES_MAX 2

typedef struct {
    LLVMTypeRef type; // The type that this info is for

    LLVMTypeRef direct_types[ABI_DIRECT_TYPES_MAX];
    size_t      direct_types_count;
} ABI_Info;

typedef struct {
    ABI_Info *args;
    size_t    args_count;

    ABI_Info return_abi;
    Type    *return_type;

    LLVMTypeRef *actual_args;
    size_t       actual_args_count;

    bool   is_variadic;
    size_t variadics_start;
} ABI;

typedef struct {
    Type        *type;
    LLVMValueRef value;
} Typed_LLVM_Value;

#define i64_from_int128(n) (n).low

// Utilities ///////////////////////////////////////////////////////////////////////////////////////
bool type_is_compound(Type type);

LLVMValueRef get_load_ptr(LLVMValueRef value);
LLVMValueRef undo_load(LLVMValueRef value);

const char     *temp_nested_fn_name(Node_Fn *fn, Module *module);
LLVMMetadataRef get_debug_file(Compiler *c, const char *path);
LLVMMetadataRef get_scope_of_definition(Compiler *c, Node *node, Node_Fn *defined_in);
void            set_debug_pos(Compiler *c, Pos pos);

LLVMValueRef compile_alloca(Compiler *c, LLVMTypeRef type);
LLVMValueRef compile_cast(Compiler *c, LLVMValueRef from, LLVMTypeRef to_type, bool is_signed);

Typed_LLVM_Value get_builtin_func(Compiler *c, SV name);
void compile_panic_v2(Compiler *c, Pos pos, Contract_Panic panic, LLVMValueRef v1, LLVMValueRef v2, LLVMValueRef v3);

// Types ///////////////////////////////////////////////////////////////////////////////////////////
LLVMTypeRef     compile_type(Compiler *c, Type *type);
LLVMTypeRef     compile_fn_type(Compiler *c, Type type, ABI *abi);
LLVMMetadataRef get_debug_for_type(Compiler *c, Type *type);

// Runtime Type Information ////////////////////////////////////////////////////////////////////////
typedef struct {
    Type *type;

    size_t        variant_index;
    LLVMValueRef *type_info;

    LLVMValueRef ti_fields[4];
    size_t       ti_fields_iota;

    LLVMValueRef tiv_fields[3];
    size_t       tiv_fields_iota;

    LLVMValueRef done;
} Type_Info_Compiler;

LLVMValueRef compile_type_info(Compiler *c, Type *type);

// ABI /////////////////////////////////////////////////////////////////////////////////////////////
ABI_Info get_abi_info_for_type(Compiler *c, Type *type, bool is_arg);

void        abi_set_return_type(Compiler *c, ABI *abi, Type *type);
void        abi_set_argument_type(Compiler *c, ABI *abi, size_t index, Type *type);
void        abi_set_variadic_at(ABI *abi, size_t index);
LLVMTypeRef abi_finalize(Compiler *c, ABI *abi);

typedef struct {
    const void *checkpoint;

    LLVMMetadataRef debug_pos;
    size_t          arg_values_start;

    ABI_Info  return_info;
    ABI_Info *args_info;
    size_t    args_count;

    Typed_LLVM_Value fn;
    const Type_Fn   *fn_spec;
} Call_Compiler;

void         compile_call_begin(Compiler *c, Call_Compiler *call, Typed_LLVM_Value fn, size_t args_count);
void         compile_call_arg(Compiler *c, Call_Compiler *call, size_t arg_index, Typed_LLVM_Value *arg);
LLVMValueRef compile_call_finalize(Compiler *c, Call_Compiler *call, bool raw, bool ref);
LLVMValueRef
compile_call(Compiler *c, Typed_LLVM_Value fn, Typed_LLVM_Value *args, size_t args_count, bool is_trait_call, bool ref);

// Constant Values /////////////////////////////////////////////////////////////////////////////////
LLVMValueRef compile_const_value_into_memory(Compiler *c, LLVMValueRef value);
LLVMValueRef compile_string_into_const_value(Compiler *c, SV sv);

LLVMValueRef create_const_slice_from_memory(Compiler *c, LLVMTypeRef element, LLVMValueRef *memory, size_t count);
LLVMValueRef create_const_struct_from_single_value_if_not_already(Compiler *c, LLVMValueRef value);

LLVMValueRef compile_const_value(Compiler *c, Const_Value value, Type type);

// Expressions /////////////////////////////////////////////////////////////////////////////////////
void         compile_trait_impl(Compiler *c, Type_Trait_Impl *impl);
LLVMValueRef compile_ident(Compiler *c, Node *n, Node_Atom *definition, bool ref);
LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn);
void compile_optional_arguments(Compiler *c, Typed_LLVM_Value *args, const Type_Fn *fn_spec, Pos caller_location);

LLVMValueRef compile_expr_atom(Compiler *c, Node_Atom *atom, bool ref);
LLVMValueRef compile_expr_unary(Compiler *c, Node_Unary *unary, bool ref);
LLVMValueRef compile_expr_binary(Compiler *c, Node_Binary *binary);
LLVMValueRef compile_expr_member(Compiler *c, Node_Member *member, bool ref);
LLVMValueRef compile_expr_interpolation(Compiler *c, Node_Interpolation *interpolation, bool ref);
LLVMValueRef compile_expr_compound(Compiler *c, Node_Compound *compound, bool ref);
LLVMValueRef compile_expr_call(Compiler *c, Node_Call *call, bool ref);
LLVMValueRef compile_expr_index(Compiler *c, Node_Index *index, bool ref);
LLVMValueRef compile_expr_impl(Compiler *c, Node *n, bool ref);
LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref);

// Statements //////////////////////////////////////////////////////////////////////////////////////
void compile_var_def(Compiler *c, Node_Atom *it);
void compile_local_var_debug(Compiler *c, Node_Atom *it, LLVMMetadataRef var_debug_type);
void compile_defers(Compiler *c, size_t from, bool rollback);

void compile_stmt_define(Compiler *c, Node_Define *define);
void compile_stmt_block(Compiler *c, Node_Block *block);
void compile_stmt_if(Compiler *c, Node_If *iff);
void compile_stmt_for(Compiler *c, Node_For *forr);
void compile_stmt_switch(Compiler *c, Node_Switch *sw);
void compile_stmt_return(Compiler *c, Node_Return *returnn);
void compile_stmt(Compiler *c, Node *n);

#endif // COMPILER_INTERNAL_H
