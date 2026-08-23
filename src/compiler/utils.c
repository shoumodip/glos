#include "../checker.h"
#include "../error.h"
#include "compiler.h"

static_assert(COUNT_TYPES == 30, "");
bool type_is_compound(Type type) {
    if (type.ref) {
        return false;
    }

    switch (type.kind) {
    case TYPE_TRAIT:
    case TYPE_UNION:
    case TYPE_STRUCT:
    case TYPE_ARRAY:
    case TYPE_DYNAMIC_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRING:
    case TYPE_GROUP:
        return true;

    case TYPE_POLYMORPH:
        unreachable();

    default:
        return false;
    }
}

LLVMValueRef get_load_ptr(LLVMValueRef value) {
    assert(LLVMGetInstructionOpcode(value) == LLVMLoad);
    assert(LLVMGetFirstUse(value) == NULL);
    return LLVMGetOperand(value, 0);
}

LLVMValueRef undo_load(LLVMValueRef value) {
    LLVMValueRef ptr = get_load_ptr(value);
    LLVMInstructionEraseFromParent(value);
    return ptr;
}

const char *temp_nested_fn_name(Node_Fn *fn, Module *module) {
    const size_t start = default_sb.count;
    sb_push_fn_name(&default_sb, fn, module);
    return arena_sb_to_cstr(&temp_arena, &default_sb, start);
}

LLVMMetadataRef get_debug_file(Compiler *c, const char *path) {
    if (!c->llvm_debug_files.hasheq) {
        c->llvm_debug_files.hasheq = ht_hasheq_cstr;
    }

    LLVMMetadataRef *metadatap = ht_get(&c->llvm_debug_files, path);
    if (metadatap) {
        return *metadatap;
    }

    LLVMMetadataRef metadata = LLVMDIBuilderCreateFile(c->llvm_debug_builder, path, strlen(path), ".", strlen("."));
    ht_set(&c->llvm_debug_files, path, metadata);
    return metadata;
}

LLVMMetadataRef get_scope_of_definition(Compiler *c, Node *node, Node_Fn *defined_in) {
    if (!defined_in) {
        return get_debug_file(c, node->token.pos.path);
    }

    assert(defined_in->llvm_debug_scope);
    return defined_in->llvm_debug_scope;
}

void set_debug_pos(Compiler *c, Pos pos) {
    LLVMSetCurrentDebugLocation2(
        c->llvm_builder,
        LLVMDIBuilderCreateDebugLocation(c->llvm_context, pos.row + 1, pos.col + 1, c->llvm_debug_scope, NULL));
}

LLVMValueRef compile_alloca(Compiler *c, LLVMTypeRef type) {
    LLVMMetadataRef debug_pos = LLVMGetCurrentDebugLocation2(c->llvm_builder);

    LLVMBasicBlockRef llvm_current_block_save = LLVMGetInsertBlock(c->llvm_builder);
    if (c->llvm_fn_last_alloca) {
        LLVMValueRef next_inst = LLVMGetNextInstruction(c->llvm_fn_last_alloca);
        if (next_inst) {
            LLVMPositionBuilderBefore(c->llvm_builder, next_inst);
        } else {
            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMGetFirstBasicBlock(c->llvm_fn));
        }
    } else {
        LLVMBasicBlockRef first_block = LLVMGetFirstBasicBlock(c->llvm_fn);
        LLVMValueRef      first_inst = LLVMGetFirstInstruction(first_block);
        if (first_inst) {
            LLVMPositionBuilderBefore(c->llvm_builder, first_inst);
        } else {
            LLVMPositionBuilderAtEnd(c->llvm_builder, first_block);
        }
    }

    LLVMValueRef alloca = LLVMBuildAlloca(c->llvm_builder, type, "");
    LLVMSetAlignment(alloca, LLVMABIAlignmentOfType(c->llvm_target_data, type));
    c->llvm_fn_last_alloca = alloca;
    LLVMPositionBuilderAtEnd(c->llvm_builder, llvm_current_block_save);

    LLVMSetCurrentDebugLocation2(c->llvm_builder, debug_pos);
    return alloca;
}

static inline bool llvm_type_kind_is_float(LLVMTypeKind kind) {
    return kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind;
}

LLVMValueRef compile_cast(Compiler *c, LLVMValueRef from, LLVMTypeRef to_type, bool is_from_signed, bool is_to_signed) {
    LLVMTypeRef from_type = LLVMTypeOf(from);
    if (from_type == to_type) {
        return from;
    }

    LLVMTypeKind from_kind = LLVMGetTypeKind(from_type);
    LLVMTypeKind to_kind = LLVMGetTypeKind(to_type);

    // Pointer -> Integer
    if (from_kind == LLVMPointerTypeKind && to_kind == LLVMIntegerTypeKind) {
        return LLVMBuildPtrToInt(c->llvm_builder, from, to_type, "");
    }

    // Integer -> Pointer
    if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMPointerTypeKind) {
        return LLVMBuildIntToPtr(c->llvm_builder, from, to_type, "");
    }

    // Integer -> Integer
    if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMIntegerTypeKind) {
        const size_t from_width = LLVMGetIntTypeWidth(from_type);
        const size_t to_width = LLVMGetIntTypeWidth(to_type);
        if (from_width > to_width) {
            return LLVMBuildTrunc(c->llvm_builder, from, to_type, "");
        } else if (from_width < to_width) {
            // Smaller -> Bigger
            if (is_from_signed) {
                return LLVMBuildSExt(c->llvm_builder, from, to_type, "");
            }
            return LLVMBuildZExt(c->llvm_builder, from, to_type, "");
        } else {
            // Bigger -> Smaller
            return LLVMBuildBitCast(c->llvm_builder, from, to_type, "");
        }
    }

    // Float -> Integer
    if (llvm_type_kind_is_float(from_kind) && to_kind == LLVMIntegerTypeKind) {
        if (is_to_signed) {
            return LLVMBuildFPToSI(c->llvm_builder, from, to_type, "");
        } else {
            return LLVMBuildFPToUI(c->llvm_builder, from, to_type, "");
        }
    }

    // Integer -> Float
    if (from_kind == LLVMIntegerTypeKind && llvm_type_kind_is_float(to_kind)) {
        if (is_from_signed) {
            return LLVMBuildSIToFP(c->llvm_builder, from, to_type, "");
        } else {
            return LLVMBuildUIToFP(c->llvm_builder, from, to_type, "");
        }
    }

    // Float -> FLoat
    if (llvm_type_kind_is_float(from_kind) && llvm_type_kind_is_float(to_kind)) {
        if (!c->llvm_target_data) {
            compiler_init_llvm_target_data(c);
        }

        const size_t from_size = LLVMABISizeOfType(c->llvm_target_data, from_type);
        const size_t to_size = LLVMABISizeOfType(c->llvm_target_data, to_type);
        if (from_size > to_size) {
            // Bigger -> Smaller
            return LLVMBuildFPTrunc(c->llvm_builder, from, to_type, "");
        } else if (from_size < to_size) {
            // Smaller -> Bigger
            return LLVMBuildFPExt(c->llvm_builder, from, to_type, "");
        } else {
            unreachable();
        }
    }

    unreachable();
}

Typed_LLVM_Value get_builtin_func(Compiler *c, SV name) {
    const Const_Value value = get_const_definition_value(c, c->builtin_module, name, NULL);
    assert(value.kind == CONST_VALUE_FN);

    Typed_LLVM_Value result = {0};
    result.value = compile_fn(c, value.as.fn);
    result.type = &value.as.fn->node.type;
    return result;
}

void compile_panic_v2(Compiler *c, Pos pos, Contract_Panic panic, LLVMValueRef v1, LLVMValueRef v2, LLVMValueRef v3) {
    Typed_LLVM_Value fn = get_builtin_func(c, sv_from_cstr("runtime_panic"));

    LLVMTypeRef  i64 = LLVMInt64TypeInContext(c->llvm_context);
    LLVMValueRef zero = LLVMConstNull(i64);

    LLVMValueRef location = compile_alloca(c, c->source_code_location_type.llvm);
    LLVMBuildStore(c->llvm_builder, compile_string_into_const_value(c, sv_from_cstr(pos.path)), location);
    LLVMBuildStore(
        c->llvm_builder,
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), pos.row + 1, true),
        LLVMBuildStructGEP2(c->llvm_builder, c->source_code_location_type.llvm, location, 1, ""));

    LLVMBuildStore(
        c->llvm_builder,
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), pos.col + 1, true),
        LLVMBuildStructGEP2(c->llvm_builder, c->source_code_location_type.llvm, location, 2, ""));

    LLVMValueRef args[] = {
        LLVMConstInt(i64, panic, true),
        v1 ? v1 : zero,
        v2 ? v2 : zero,
        v3 ? v3 : zero,
        location,
    };

    LLVMBuildCall2(c->llvm_builder, fn.type->llvm, fn.value, args, len(args), "");
    LLVMBuildUnreachable(c->llvm_builder);
}

void compiler_init_llvm_target_data(Compiler *c) {
    if (LLVMInitializeNativeTarget() != 0) {
        error_standalone(EK_ERROR, "Failed to initialize native target");
        exit(1);
    }
    LLVMInitializeNativeAsmPrinter();

    c->llvm_context = LLVMContextCreate();
    c->llvm_module = LLVMModuleCreateWithNameInContext("", c->llvm_context);

    char *triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(c->llvm_module, triple);

    char *error = NULL;

    LLVMTargetRef target = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
        error_standalone(EK_ERROR, "%s", error);
        exit(1);
    }

    LLVMCodeGenOptLevel opt_level;
    switch (c->optimization_level) {
    case O0:
        opt_level = LLVMCodeGenLevelNone;
        break;

    case O1:
        opt_level = LLVMCodeGenLevelLess;
        break;

    case O2:
        opt_level = LLVMCodeGenLevelDefault;
        break;

    case O3:
        opt_level = LLVMCodeGenLevelAggressive;
        break;

    default:
        unreachable();
        break;
    }

    c->llvm_target_machine =
        LLVMCreateTargetMachine(target, triple, "generic", "", opt_level, LLVMRelocPIC, LLVMCodeModelDefault);
    c->llvm_target_data = LLVMCreateTargetDataLayout(c->llvm_target_machine);

    // Initialize the common types
    {
        LLVMTypeRef dynamic_array_fields[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMInt64TypeInContext(c->llvm_context),
            LLVMInt64TypeInContext(c->llvm_context),
        };
        c->llvm_dynamic_array_type =
            LLVMStructTypeInContext(c->llvm_context, dynamic_array_fields, len(dynamic_array_fields), false);

        LLVMTypeRef slice_fields[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMInt64TypeInContext(c->llvm_context),
        };
        c->llvm_slice_type = LLVMStructTypeInContext(c->llvm_context, slice_fields, len(slice_fields), false);

        LLVMTypeRef trait_fields[] = {
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMPointerTypeInContext(c->llvm_context, 0),
        };
        c->llvm_trait_type = LLVMStructTypeInContext(c->llvm_context, trait_fields, len(trait_fields), false);
    }

    free(triple);
}
