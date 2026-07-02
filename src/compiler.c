#include "compiler.h"
#include "basic.h"
#include "checker.h"
#include "dwarf.h"
#include "node.h"
#include "token.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

void link_flags_add_libpath(Link_Flags *ls, SV path) {
#ifdef PLATFORM_X86_64_WINDOWS
    da_push(ls, arena_sprintf(&default_arena, "/libpath:" SV_Fmt, SV_Arg(path)));
#else
    da_push(ls, arena_sprintf(&default_arena, "-L" SV_Fmt, SV_Arg(path)));
#endif // PLATFORM_X86_64_WINDOWS
}

void link_flags_add_libname(Link_Flags *ls, SV name) {
#ifdef PLATFORM_X86_64_WINDOWS
    da_push(ls, arena_sprintf(&default_arena, SV_Fmt ".lib", SV_Arg(name)));
#else
    da_push(ls, arena_sprintf(&default_arena, "-l" SV_Fmt, SV_Arg(name)));
#endif // PLATFORM_X86_64_WINDOWS
}

static_assert(COUNT_TYPES == 25, "");
static LLVMTypeRef compile_type(Compiler *c, Type *type) {
    if (!type) {
        return NULL;
    }

    if (type->llvm) {
        return type->llvm;
    }

    assert(type->kind != TYPE_MODULE);
    assert(type->kind != TYPE_UNKNOWN_ENUM);
    assert(type->kind != TYPE_UNKNOWN_COMPOUND);

    // NOTE: Do not use `type*` functions because this function should not care whether a type is a metatype or not.
    if (type->ref) {
        type->llvm = LLVMPointerTypeInContext(c->llvm_context, 0);
        return type->llvm;
    }

    switch (type->kind) {
    case TYPE_UNIT:
        type->llvm = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case TYPE_BOOL:
        type->llvm = LLVMInt1TypeInContext(c->llvm_context);
        break;

    case TYPE_I8:
    case TYPE_U8:
    case TYPE_CHAR:
        type->llvm = LLVMInt8TypeInContext(c->llvm_context);
        break;

    case TYPE_I16:
    case TYPE_U16:
        type->llvm = LLVMInt16TypeInContext(c->llvm_context);
        break;

    case TYPE_I32:
    case TYPE_U32:
        type->llvm = LLVMInt32TypeInContext(c->llvm_context);
        break;

    case TYPE_I64:
    case TYPE_U64:
    case TYPE_INT:
        type->llvm = LLVMInt64TypeInContext(c->llvm_context);
        break;

    case TYPE_RAWPTR:
    case TYPE_FN:
        type->llvm = LLVMPointerTypeInContext(c->llvm_context, 0);
        break;

    case TYPE_ENUM: {
        Node_Enum *definition = type->spec.enumm.definition;
        if (!definition->llvm) {
            Type stub = {.kind = type->spec.enumm.underlying};
            compile_type(c, &stub);
            definition->llvm = stub.llvm;
        }
        type->llvm = definition->llvm;
    } break;

    case TYPE_UNION: {
        Type_Union *spec = type->spec.unionn;
        if (!spec->llvm) {
            for (size_t i = 0; i < spec->variants_count; i++) {
                Type_Union_Variant *it = &spec->variants[i];
                compile_type(c, &it->type);

                it->size = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm);
                spec->variants_size_max = max(spec->variants_size_max, it->size);

                it->align = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm);
                spec->variants_align_max = max(spec->variants_align_max, it->align);
            }

            LLVMTypeRef fields[] = {
                LLVMInt64TypeInContext(c->llvm_context),
                LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), spec->variants_size_max),
            };

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, len(fields), false);
        }
        type->llvm = spec->llvm;
    } break;

    case TYPE_STRUCT: {
        assert(type->spec.structt);

        Type_Struct *spec = type->spec.structt;
        if (!spec->llvm) {
            LLVMTypeRef *fields = arena_alloc(&temp_arena, spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                compile_type(c, &it->type);
                fields[i] = it->type.llvm;
            }

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec->fields_count, false);
            arena_reset(&temp_arena, fields);

            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                it->offset = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i);
            }
        }
        type->llvm = spec->llvm;
    } break;

    case TYPE_ARRAY:
        compile_type(c, type->spec.array.element);
        type->llvm = LLVMArrayType(type->spec.array.element->llvm, type->spec.array.count);
        break;

    case TYPE_SLICE:
    case TYPE_STRING:
        type->llvm = c->llvm_slice_type;
        break;

    case TYPE_TRAIT:
        type->llvm = c->llvm_trait_type;
        break;

    case TYPE_GROUP: {
        Type_Group *spec = &type->spec.group;
        if (!spec->llvm) {
            LLVMTypeRef *fields = arena_alloc(&temp_arena, spec->count * sizeof(*fields));
            for (size_t i = 0; i < spec->count; i++) {
                Type *it = &spec->data[i];
                compile_type(c, it);
                fields[i] = it->llvm;
            }

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec->count, false);
            arena_reset(&temp_arena, fields);

            spec->offsets = arena_alloc(&default_arena, spec->count * sizeof(*spec->offsets));
            for (size_t i = 0; i < spec->count; i++) {
                spec->offsets[i] = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i);
            }
        }
        type->llvm = spec->llvm;
    } break;

    default:
        unreachable();
        break;
    }

    return type->llvm;
}

static LLVMValueRef compile_alloca(Compiler *c, LLVMTypeRef type) {
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

static LLVMValueRef compile_cast(Compiler *c, LLVMValueRef from, LLVMTypeRef to_type, bool is_signed) {
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
            if (is_signed) {
                return LLVMBuildSExt(c->llvm_builder, from, to_type, "");
            }
            return LLVMBuildZExt(c->llvm_builder, from, to_type, "");
        } else {
            // Bigger -> Smaller
            return LLVMBuildBitCast(c->llvm_builder, from, to_type, "");
        }
    }

    unreachable();
}

#define ABI_DIRECT_TYPES_MAX 2

typedef struct {
    LLVMTypeRef type; // The type that this info is for

    LLVMTypeRef direct_types[ABI_DIRECT_TYPES_MAX];
    size_t      direct_types_count;
} ABI_Info;

static_assert(COUNT_TYPES == 25, "");
static bool type_is_compound(Type type) {
    if (type.ref) {
        return false;
    }

    switch (type.kind) {
    case TYPE_TRAIT:
    case TYPE_UNION:
    case TYPE_STRUCT:
    case TYPE_ARRAY:
    case TYPE_SLICE:
    case TYPE_STRING:
    case TYPE_GROUP:
        return true;

    default:
        return false;
    }
}

#ifdef PLATFORM_X86_64_LINUX
static_assert(COUNT_TYPES == 25, "");
static void x86_64_linux_split_into_two(Compiler *c, Type type, size_t offset, LLVMTypeRef out[2]) {
    assert(type_is_compound(type));
    switch (type.kind) {
    case TYPE_STRUCT: {
        const Type_Struct *spec = type.spec.structt;
        for (size_t i = 0; i < spec->fields_count; i++) {
            const Type_Struct_Field *it = &spec->fields[i];
            const size_t             it_offset = offset + it->offset;
            if (type_is_compound(it->type)) {
                x86_64_linux_split_into_two(c, it->type, it_offset, out);
            } else {
                const size_t index = it_offset / 8;
                assert(index < 2);
                if (out[index]) {
                    out[index] = LLVMInt64TypeInContext(c->llvm_context);
                } else {
                    out[index] = LLVMIntTypeInContext(
                        c->llvm_context, LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8);
                }
            }
        }
    } break;

    case TYPE_UNION:
    case TYPE_ARRAY:
        out[0] = LLVMInt64TypeInContext(c->llvm_context);
        out[1] = LLVMIntTypeInContext(c->llvm_context, compile_sizeof(c, &type) * 8 - 64);
        break;

    case TYPE_SLICE:
    case TYPE_STRING:
        out[0] = LLVMInt64TypeInContext(c->llvm_context);
        out[1] = LLVMInt64TypeInContext(c->llvm_context);
        break;

    case TYPE_GROUP: {
        const Type_Group *spec = &type.spec.group;
        for (size_t i = 0; i < spec->count; i++) {
            const Type   it_type = spec->data[i];
            const size_t it_offset = offset + spec->offsets[i];
            if (type_is_compound(it_type)) {
                x86_64_linux_split_into_two(c, it_type, it_offset, out);
            } else {
                const size_t index = it_offset / 8;
                assert(index < 2);
                if (out[index]) {
                    out[index] = LLVMInt64TypeInContext(c->llvm_context);
                } else {
                    out[index] =
                        LLVMIntTypeInContext(c->llvm_context, LLVMABISizeOfType(c->llvm_target_data, it_type.llvm) * 8);
                }
            }
        }
    } break;

    default:
        unreachable();
    }
}
#endif // PLATFORM_X86_64_LINUX

static ABI_Info get_abi_info_for_type(Compiler *c, Type *type, bool is_arg) {
    ABI_Info info = {0};
    size_t   size = compile_sizeof(c, type);

    info.type = type->llvm;
    if (type->ref) {
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;
    }

    static_assert(COUNT_TYPES == 25, "");
    switch (type->kind) {
    case TYPE_UNIT:
        info.direct_types[info.direct_types_count++] = LLVMVoidTypeInContext(c->llvm_context);
        return info;

    case TYPE_BOOL:
        info.direct_types[info.direct_types_count++] = LLVMInt1TypeInContext(c->llvm_context);
        return info;

    case TYPE_RAWPTR:
    case TYPE_FN:
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;

    default:
        // Pass
        break;
    }

    if (size <= 8) {
#ifdef PLATFORM_ARM64_MACOS
        if (is_arg && type_is_compound(*type)) {
            size = 8;
        }
#endif // PLATFORM_ARM64_MACOS

        info.direct_types[info.direct_types_count++] = LLVMIntTypeInContext(c->llvm_context, size * 8);
        return info;
    }

#ifdef PLATFORM_X86_64_LINUX
    if (size <= 16) {
        x86_64_linux_split_into_two(c, *type, 0, info.direct_types);
        assert(info.direct_types[0]);
        assert(info.direct_types[1]);
        info.direct_types_count = 2;
        return info;
    }
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    if (size <= 16) {
        info.direct_types[info.direct_types_count++] = LLVMArrayType(LLVMInt64TypeInContext(c->llvm_context), 2);
        return info;
    }
#endif // PLATFORM_ARM64_MACOS

    return info;
    unused(is_arg); // Suppress the unused warnings
}

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

static void abi_set_return_type(Compiler *c, ABI *abi, Type *type) {
    assert(abi->actual_args_count == 0);
    abi->return_abi = get_abi_info_for_type(c, type, false);
    abi->return_type = type;
    if (!abi->return_abi.direct_types_count) {
        abi->actual_args_count++;
    }
}

static void abi_set_argument_type(Compiler *c, ABI *abi, size_t index, Type *type) {
    assert(index < abi->args_count);
    ABI_Info *it = &abi->args[index];
    *it = get_abi_info_for_type(c, type, true);
    if (it->direct_types_count) {
        abi->actual_args_count += it->direct_types_count;
    } else {
        abi->actual_args_count++;
    }
}

static void abi_set_variadic_at(ABI *abi, size_t index) {
    assert(!abi->is_variadic);
    abi->is_variadic = true;
    abi->variadics_start = index;
}

static LLVMTypeRef abi_finalize(Compiler *c, ABI *abi) {
    size_t args_iota = 0;
    if (!abi->return_abi.direct_types_count) {
        abi->actual_args[args_iota++] = LLVMPointerTypeInContext(c->llvm_context, 0);
    }

    for (size_t i = 0; i < abi->args_count; i++) {
        const ABI_Info it_abi = abi->args[i];
        if (it_abi.direct_types_count) {
            for (size_t j = 0; j < it_abi.direct_types_count; j++) {
                abi->actual_args[args_iota++] = it_abi.direct_types[j];
            }
        } else {
            abi->actual_args[args_iota++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        }
    }

    LLVMTypeRef return_type = NULL;

    static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
    switch (abi->return_abi.direct_types_count) {
    case 0:
        return_type = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case 1:
        return_type = abi->return_abi.direct_types[0];
        break;

    case 2:
        return_type = LLVMStructTypeInContext(
            c->llvm_context, abi->return_abi.direct_types, abi->return_abi.direct_types_count, false);
        break;

    default:
        unreachable();
    }

    return LLVMFunctionType(
        return_type,
        abi->actual_args,
        abi->is_variadic ? abi->variadics_start : abi->actual_args_count,
        abi->is_variadic);
}

static LLVMValueRef undo_load(LLVMValueRef value) {
    assert(LLVMGetInstructionOpcode(value) == LLVMLoad);
    assert(LLVMGetFirstUse(value) == NULL);
    LLVMValueRef ptr = LLVMGetOperand(value, 0);
    LLVMInstructionEraseFromParent(value);
    return ptr;
}

// The ABI should be stored into the function itself to prevent recomputation on every call
static LLVMTypeRef compile_fn_type(Compiler *c, Type type, ABI *abi) {
    assert(!type.ref && type.kind == TYPE_FN);
    Type_Fn *spec = type.spec.fn;

    const void *checkpoint = arena_alloc(&temp_arena, 0);

    abi_set_return_type(c, abi, spec->return_type);
    for (size_t i = 0; i < spec->args_count; i++) {
        abi_set_argument_type(c, abi, i, &spec->args[i].type);
    }

    if (spec->variadics_kind == VARIADICS_UNTYPED) {
        abi_set_variadic_at(abi, abi->actual_args_count);
    }

    abi->actual_args = arena_alloc(&temp_arena, abi->actual_args_count * sizeof(*abi->actual_args));
    spec->llvm = abi_finalize(c, abi);

    arena_reset(&temp_arena, checkpoint);
    return spec->llvm;
}

static LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn);
static LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref);
static void         compile_stmt(Compiler *c, Node *n);

static void sb_push_nested_fn_name(Compiler *c, SB *sb, Node_Fn *fn, Module *module) {
    if (!fn) {
        sb_push_sv(sb, module->name);
        return;
    }

    if (fn->wrapper) {
        assert(fn->defined_as && !fn->outer_fn && fn->wrapper_for_trait);

        Node_Trait *definition = fn->wrapper_for_trait->definition;
        sb_push_nested_fn_name(c, sb, definition->defined_in, definition->module);
        sb_push(sb, '.');
        sb_push_type(sb, (Type) {.kind = TYPE_TRAIT, .spec.trait = fn->wrapper_for_trait});
        sb_push(sb, '(');
        sb_push_nested_fn_name(c, sb, fn->wrapper, fn->module);
        sb_push(sb, ')');
        return;
    }

    sb_push_nested_fn_name(c, sb, fn->outer_fn, module);
    if (fn->is_method) {
        assert(fn->defined_as);
        assert(!fn->outer_fn);

        assert(fn->node.type.kind == TYPE_FN);
        const Type_Fn *fn_spec = fn->node.type.spec.fn;

        assert(fn_spec->args_count);
        sb_sprintf(sb, ".");

        Type receiver = fn_spec->args[0].type;
        receiver.ref = 0;
        sb_push_type(sb, receiver);
    }

    if (fn->defined_as) {
        sb_sprintf(sb, "." SV_Fmt, SV_Arg(fn->defined_as->node.token.sv));
    } else {
        if (!fn->defined_as_anon_iota) {
            fn->defined_as_anon_iota = ++c->iota_anonymous_fn;
        }
        sb_sprintf(sb, ".anon.%zu", fn->defined_as_anon_iota);
    }
}

static const char *temp_nested_fn_name(Compiler *c, Node_Fn *fn, Module *module) {
    const size_t start = default_sb.count;
    sb_push_nested_fn_name(c, &default_sb, fn, module);
    return arena_sb_to_cstr(&temp_arena, &default_sb, start);
}

static LLVMMetadataRef get_debug_file(Compiler *c, const char *path) {
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

static LLVMMetadataRef get_scope_of_definition(Compiler *c, Node *node, Node_Fn *defined_in) {
    if (!defined_in) {
        return get_debug_file(c, node->token.pos.path);
    }

    assert(defined_in->llvm_debug_scope);
    return defined_in->llvm_debug_scope;
}

static LLVMMetadataRef get_debug_for_type(Compiler *c, Type *type);

// Assertion: sizeof(type) == 8
typedef struct {
    SV   name;
    Type type;
} Builtin_Compound_Type_Field;

static LLVMMetadataRef
get_debug_for_builtin_compound_type(Compiler *c, SV name, Builtin_Compound_Type_Field *fields, size_t fields_count) {
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    LLVMMetadataRef  empty_file_path_metadata = get_debug_file(c, "");
    LLVMMetadataRef *members = arena_alloc(&temp_arena, fields_count * sizeof(*members));

    size_t size_bits = 0;
    for (size_t i = 0; i < fields_count; i++) {
        Builtin_Compound_Type_Field it = fields[i];
        assert(compile_sizeof(c, &it.type) == 8);

        members[i] = LLVMDIBuilderCreateMemberType(
            c->llvm_debug_builder,
            c->llvm_debug_compile_unit,
            it.name.data,
            it.name.count,
            empty_file_path_metadata,
            0,
            64,
            64,
            size_bits,
            0,
            get_debug_for_type(c, &it.type));

        size_bits += 64;
    }

    LLVMMetadataRef real_metadata = LLVMDIBuilderCreateStructType(
        c->llvm_debug_builder,
        c->llvm_debug_compile_unit,
        "",
        0,
        empty_file_path_metadata,
        0,
        size_bits,
        64,
        0,
        NULL,
        members,
        fields_count,
        0,
        NULL,
        "",
        0);

    LLVMMetadataRef typedef_metadata = LLVMDIBuilderCreateTypedef(
        c->llvm_debug_builder,
        real_metadata,
        name.data,
        name.count,
        empty_file_path_metadata,
        0,
        c->llvm_debug_compile_unit,
        64);

    arena_reset(&temp_arena, checkpoint);
    return typedef_metadata;
}

static_assert(COUNT_TYPES == 25, "");
static LLVMMetadataRef get_debug_for_type(Compiler *c, Type *type) {
    assert(!type->is_meta);
    if (type->ref) {
        Type inner = *type;
        inner.ref--;
        inner.llvm = NULL;
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, get_debug_for_type(c, &inner), sizeof(void *), sizeof(void *), 0, "", 0);
    }

    switch (type->kind) {
    case TYPE_UNIT:
        return NULL;

    case TYPE_BOOL:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "bool", strlen("bool"), 8, DW_ATE_boolean, 0);

    case TYPE_CHAR:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "char", strlen("char"), 8, DW_ATE_unsigned_char, 0);

    case TYPE_I8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i8", strlen("i8"), 8, DW_ATE_signed, 0);

    case TYPE_I16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i16", strlen("i16"), 16, DW_ATE_signed, 0);

    case TYPE_I32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i32", strlen("i32"), 32, DW_ATE_signed, 0);

    case TYPE_I64:
    case TYPE_INT:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "i64", strlen("i64"), 64, DW_ATE_signed, 0);

    case TYPE_U8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u8", strlen("u8"), 8, DW_ATE_unsigned, 0);

    case TYPE_U16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u16", strlen("u16"), 16, DW_ATE_unsigned, 0);

    case TYPE_U32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u32", strlen("u32"), 32, DW_ATE_unsigned, 0);

    case TYPE_U64:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u64", strlen("u64"), 64, DW_ATE_unsigned, 0);

    case TYPE_RAWPTR:
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, NULL, sizeof(void *), sizeof(void *), 0, "rawptr", strlen("rawptr"));

    case TYPE_FN: {
        const Type_Fn *spec = type->spec.fn;

        LLVMMetadataRef *args = arena_alloc(&temp_arena, (spec->args_count + 1) * sizeof(*args));
        args[0] = get_debug_for_type(c, spec->return_type);
        for (size_t i = 0; i < spec->args_count; i++) {
            args[i + 1] = get_debug_for_type(c, &spec->args[i].type);
        }

        LLVMMetadataRef fn_debug_type =
            LLVMDIBuilderCreateSubroutineType(c->llvm_debug_builder, NULL, args, spec->args_count + 1, 0);

        arena_reset(&temp_arena, args);
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, fn_debug_type, sizeof(void *), sizeof(void *), 0, "", 0);
    }

    case TYPE_ENUM: {
        Node_Enum *definition = type->spec.enumm.definition;
        if (!definition->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            const size_t size = compile_sizeof(c, type);
            const SV     name = sv_from_cstr(type_to_cstr(*type));
            definition->debug = LLVMDIBuilderCreateBasicType(
                c->llvm_debug_builder,
                name.data,
                name.count,
                size * 8,
                type_is_signed(*type) ? DW_ATE_signed : DW_ATE_unsigned,
                0);

            arena_reset(&temp_arena, checkpoint);
        }
        return definition->debug;
    }

    case TYPE_TRAIT: {
        compile_type(c, type);

        Type_Trait *spec = type->spec.trait;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            SV name = {0};
            {
                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    const size_t start = default_sb.count;
                    sb_push_nested_fn_name(c, &default_sb, spec->definition->defined_in, spec->definition->module);
                    sb_sprintf(&default_sb, "." SV_Fmt, SV_Arg(defined_as->node.token.sv));
                    name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                }
            }

            Builtin_Compound_Type_Field fields[3] = {0};
            fields[0].name = sv_from_cstr("type");
            fields[0].type = c->type_info_pointer_type;

            fields[1].name = sv_from_cstr("data");
            fields[1].type = (Type) {.kind = TYPE_RAWPTR};

            fields[2].name = sv_from_cstr("impl");
            fields[2].type = (Type) {.kind = TYPE_RAWPTR};

            spec->debug = get_debug_for_builtin_compound_type(c, name, fields, len(fields));
            arena_reset(&temp_arena, checkpoint);
        }

        return spec->debug;
    }

    case TYPE_UNION: {
        compile_type(c, type);

        Type_Union *spec = type->spec.unionn;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            SV name = {0};
            {
                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    const size_t start = default_sb.count;
                    sb_push_nested_fn_name(c, &default_sb, spec->definition->defined_in, spec->definition->module);
                    sb_sprintf(&default_sb, "." SV_Fmt, SV_Arg(defined_as->node.token.sv));
                    name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                }
            }

            LLVMMetadataRef scope_metadata = NULL;
            if (spec->definition->defined_in) {
                scope_metadata = get_scope_of_definition(c, (Node *) spec->definition, spec->definition->defined_in);
            }

            LLVMMetadataRef file_metadata = get_debug_file(c, spec->definition->node.token.pos.path);
            LLVMMetadataRef fields[2];

            spec->debug = LLVMDIBuilderCreateReplaceableCompositeType(
                c->llvm_debug_builder,
                DW_TAG_structure_type,
                name.data,
                name.count,
                scope_metadata,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                0,
                0,
                0,
                0,
                NULL,
                0);

            // Case
            {
                Type            case_type = {.kind = TYPE_I64};
                LLVMMetadataRef case_type_metadata = get_debug_for_type(c, &case_type);

                fields[0] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    "case",
                    strlen("case"),
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    64,
                    64,
                    0,
                    0,
                    case_type_metadata);
            }

            // Payload
            {
                LLVMMetadataRef forward = LLVMDIBuilderCreateReplaceableCompositeType(
                    c->llvm_debug_builder,
                    DW_TAG_union_type,
                    name.data,
                    name.count,
                    spec->debug,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    0,
                    0,
                    0,
                    0,
                    NULL,
                    0);

                LLVMMetadataRef *variants = arena_alloc(&temp_arena, spec->variants_count * sizeof(*variants));
                for (size_t i = 0; i < spec->variants_count; i++) {
                    Type_Union_Variant *it = &spec->variants[i];

                    const SV     name = sv_from_cstr(type_to_cstr_raw(it->type));
                    const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                    const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;

                    variants[i] = LLVMDIBuilderCreateMemberType(
                        c->llvm_debug_builder,
                        forward,
                        name.data,
                        name.count,
                        file_metadata,
                        it->pos.row + 1,
                        size_bits,
                        align_bits,
                        0,
                        0,
                        get_debug_for_type(c, &it->type));
                }

                LLVMMetadataRef real = LLVMDIBuilderCreateUnionType(
                    c->llvm_debug_builder,
                    spec->debug,
                    "",
                    0,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    spec->variants_size_max * 8,
                    spec->variants_align_max * 8,
                    0,
                    variants,
                    spec->variants_count,
                    0,
                    "",
                    0);

                LLVMMetadataReplaceAllUsesWith(forward, real);
                fields[1] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    "",
                    0,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    spec->variants_size_max * 8,
                    spec->variants_align_max * 8,
                    64,
                    0,
                    real);
            }

            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                scope_metadata,
                name.data,
                name.count,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                len(fields),
                0,
                NULL,
                "",
                0);

            LLVMMetadataReplaceAllUsesWith(spec->debug, real);
            if (spec->definition->defined_as) {
                real = LLVMDIBuilderCreateTypedef(
                    c->llvm_debug_builder,
                    real,
                    name.data,
                    name.count,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    scope_metadata,
                    LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);
            }

            spec->debug = real;
            arena_reset(&temp_arena, checkpoint);
        }
        return spec->debug;
    }

    case TYPE_STRUCT: {
        compile_type(c, type);

        Type_Struct *spec = type->spec.structt;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            SV name = {0};
            {
                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    const size_t start = default_sb.count;
                    sb_push_nested_fn_name(c, &default_sb, spec->definition->defined_in, spec->definition->module);
                    sb_sprintf(&default_sb, "." SV_Fmt, SV_Arg(defined_as->node.token.sv));
                    name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                }
            }

            LLVMMetadataRef scope_metadata = NULL;
            if (spec->definition->defined_in) {
                scope_metadata = get_scope_of_definition(c, (Node *) spec->definition, spec->definition->defined_in);
            }

            spec->debug = LLVMDIBuilderCreateReplaceableCompositeType(
                c->llvm_debug_builder,
                DW_TAG_structure_type,
                name.data,
                name.count,
                scope_metadata,
                get_debug_file(c, spec->definition->node.token.pos.path),
                spec->definition->node.token.pos.row + 1,
                0,
                0,
                0,
                0,
                NULL,
                0);

            LLVMMetadataRef *fields = arena_alloc(&temp_arena, spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t offset_bits = it->offset * 8;

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    it->name.data,
                    it->name.count,
                    get_debug_file(c, it->pos.path),
                    it->pos.row + 1,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, &it->type));
            }

            LLVMMetadataRef file_metadata = get_debug_file(c, spec->definition->node.token.pos.path);
            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                scope_metadata,
                name.data,
                name.count,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                spec->fields_count,
                0,
                NULL,
                "",
                0);

            LLVMMetadataReplaceAllUsesWith(spec->debug, real);
            if (spec->definition->defined_as) {
                real = LLVMDIBuilderCreateTypedef(
                    c->llvm_debug_builder,
                    real,
                    name.data,
                    name.count,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    scope_metadata,
                    LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);
            }

            spec->debug = real;
            arena_reset(&temp_arena, checkpoint);
        }
        return spec->debug;
    }

    case TYPE_ARRAY: {
        compile_type(c, type);

        size_t subscripts_count = 0;
        for (Type *t = type; t->kind == TYPE_ARRAY; t = t->spec.array.element) {
            subscripts_count++;
        }
        LLVMMetadataRef *subscripts = arena_alloc(&temp_arena, subscripts_count * sizeof(*subscripts));

        Type  *innermost = NULL;
        size_t subscripts_iota = 0;
        for (innermost = type; innermost->kind == TYPE_ARRAY; innermost = innermost->spec.array.element) {
            subscripts[subscripts_iota++] =
                LLVMDIBuilderGetOrCreateSubrange(c->llvm_debug_builder, 0, innermost->spec.array.count);
        }

        LLVMMetadataRef metadata = LLVMDIBuilderCreateArrayType(
            c->llvm_debug_builder,
            compile_sizeof(c, type) * 8,
            0,
            get_debug_for_type(c, innermost),
            subscripts,
            subscripts_count);

        arena_reset(&temp_arena, subscripts);
        return metadata;
    } break;

    case TYPE_SLICE: {
        const void *checkpoint = arena_alloc(&temp_arena, 0);

        SV name = sv_from_cstr(type_to_cstr_raw(*type));

        Builtin_Compound_Type_Field fields[2] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = *type->spec.slice.element;
        fields[0].type.ref++;
        fields[0].type.llvm = NULL;

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_I64};

        LLVMMetadataRef metadata = get_debug_for_builtin_compound_type(c, name, fields, len(fields));
        arena_reset(&temp_arena, checkpoint);
        return metadata;
    }

    case TYPE_STRING: {
        Builtin_Compound_Type_Field fields[2] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = (Type) {.kind = TYPE_CHAR, .ref = 1};

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_I64};
        return get_debug_for_builtin_compound_type(c, sv_from_cstr("string"), fields, len(fields));
    }

    case TYPE_GROUP: {
        compile_type(c, type);

        Type_Group *spec = &type->spec.group;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);
            const SV    name = sv_from_cstr(type_to_cstr_raw(*type));

            LLVMMetadataRef empty_file_path_metadata = get_debug_file(c, "");

            LLVMMetadataRef *fields = arena_alloc(&temp_arena, spec->count * sizeof(*fields));
            for (size_t i = 0; i < spec->count; i++) {
                Type *it = &spec->data[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->llvm) * 8;
                const size_t offset_bits = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i) * 8;
                const SV     name = sv_from_cstr(arena_sprintf(&temp_arena, "%zu", i));

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    c->llvm_debug_compile_unit,
                    name.data,
                    name.count,
                    empty_file_path_metadata,
                    0,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, it));
            }

            spec->debug = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                c->llvm_debug_compile_unit,
                "",
                0,
                empty_file_path_metadata,
                0,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                spec->count,
                0,
                NULL,
                "",
                0);

            spec->debug = LLVMDIBuilderCreateTypedef(
                c->llvm_debug_builder,
                spec->debug,
                name.data,
                name.count,
                empty_file_path_metadata,
                0,
                c->llvm_debug_compile_unit,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);

            arena_reset(&temp_arena, checkpoint);
        }

        return spec->debug;
    }

    case TYPE_MODULE:
        unreachable();

    case TYPE_UNKNOWN_ENUM:
        unreachable();

    case TYPE_UNKNOWN_COMPOUND:
        unreachable();

    default:
        unreachable();
        break;
    }
}

static void set_debug_pos(Compiler *c, Pos pos) {
    LLVMSetCurrentDebugLocation2(
        c->llvm_builder,
        LLVMDIBuilderCreateDebugLocation(c->llvm_context, pos.row + 1, pos.col + 1, c->llvm_debug_scope, NULL));
}

static LLVMValueRef compile_const_value_into_memory(Compiler *c, LLVMValueRef value) {
    LLVMValueRef memory = LLVMAddGlobal(c->llvm_module, LLVMTypeOf(value), "");
    LLVMSetInitializer(memory, value);
    return memory;
}

static LLVMValueRef compile_string_into_const_value(Compiler *c, SV sv) {
    LLVMValueRef memory = LLVMConstStringInContext(c->llvm_context, sv.data, sv.count, false);

    LLVMValueRef fields[] = {
        compile_const_value_into_memory(c, memory),
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), sv.count, true),
    };
    return LLVMConstStructInContext(c->llvm_context, fields, len(fields), false);
}

static void         compile_var_def(Compiler *c, Node_Atom *it);
static LLVMValueRef compile_type_info(Compiler *c, Type *type);

#define i64_from_int128(n) (n).low

static void compile_trait_impl(Compiler *c, Type_Trait_Impl *impl) {
    if (!impl->llvm) {
        if (impl->methods_count) {
            for (size_t i = 0; i < impl->methods_count; i++) {
                Type_Trait_Impl_Method *it = &impl->methods[i];
                compile_fn(c, it->fn);

                if (!it->wrapper) {
                    const void *checkpoint = arena_alloc(&temp_arena, 0);
                    it->wrapper = compile_fn(c, create_trait_method_wrapper(&temp_arena, it->fn, impl->trait, i));
                    arena_reset(&temp_arena, checkpoint);
                }
            }

            // Checking again, since the 'compile_fn' calls might have generated this already
            if (!impl->llvm) {
                LLVMValueRef *fns = arena_alloc(&temp_arena, impl->methods_count * sizeof(*fns));
                for (size_t i = 0; i < impl->methods_count; i++) {
                    fns[i] = impl->methods[i].wrapper;
                }

                impl->llvm = compile_const_value_into_memory(
                    c, LLVMConstArray(LLVMPointerTypeInContext(c->llvm_context, 0), fns, impl->methods_count));
                arena_reset(&temp_arena, fns);
            }
        } else {
            impl->llvm = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }
    }
}

static_assert(COUNT_CONST_VALUES == 10, "");
static LLVMValueRef compile_const_value(Compiler *c, Const_Value value, Type type) {
    switch (value.kind) {
    case CONST_VALUE_INT:
        if (type_is_pointer(type)) {
            assert(int128_is_zero(value.as.integer));
            return LLVMConstNull(type.llvm);
        }
        return LLVMConstInt(type.llvm, i64_from_int128(value.as.integer), type_is_signed(type));

    case CONST_VALUE_FN:
        return compile_fn(c, value.as.fn);

    case CONST_VALUE_VAR: {
        Node_Atom *var = value.as.var;
        if (!var->definition_spec->llvm) {
            compile_var_def(c, var);
        }

        assert(var->definition_spec->llvm);
        return var->definition_spec->llvm;
    } break;

    case CONST_VALUE_TYPE:
        return compile_type_info(c, &value.as.type);

    case CONST_VALUE_TRAIT: {
        Type_Trait_Impl *impl = value.as.trait.impl;
        if (impl) {
            compile_trait_impl(c, impl);
        }

        LLVMValueRef fields[3] = {0};
        size_t       fields_iota = 0;

        // Type
        if (value.as.trait.type) {
            fields[fields_iota++] = compile_type_info(c, value.as.trait.type);
        } else {
            fields[fields_iota++] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }

        // Data
        if (value.as.trait.data) {
            fields[fields_iota++] =
                compile_const_value_into_memory(c, compile_const_value(c, *value.as.trait.data, *value.as.trait.type));
        } else {
            fields[fields_iota++] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }

        // Impl
        if (impl) {
            fields[fields_iota++] = impl->llvm;
        } else {
            fields[fields_iota++] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }
        return LLVMConstStructInContext(c->llvm_context, fields, fields_iota, false);
    }

    case CONST_VALUE_UNION: {
        const Type_Union *spec = value.as.unionn.spec;

        LLVMValueRef fields[3] = {0};
        size_t       fields_iota = 0;
        fields[fields_iota++] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), value.as.unionn.index, true);

        size_t real_size = 0;
        if (value.as.unionn.index) {
            Type type = spec->variants[value.as.unionn.index - 1].type;
            real_size = compile_sizeof(c, &type);

            assert(value.as.unionn.real); // The index is not zero, that means a real value exists
            fields[fields_iota++] = compile_const_value(c, *value.as.unionn.real, type);
        }

        const size_t padding = LLVMABISizeOfType(c->llvm_target_data, spec->llvm) - 8 - real_size;
        if (padding) {
            fields[fields_iota++] = LLVMConstNull(LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), padding));
        }

        return LLVMConstStructInContext(c->llvm_context, fields, fields_iota, false);
    }

    case CONST_VALUE_STRUCT: {
        const Type_Struct *spec = value.as.structt.spec;

        LLVMValueRef *fields = arena_alloc(&temp_arena, spec->fields_count * sizeof(*fields));
        for (size_t i = 0; i < spec->fields_count; i++) {
            fields[i] = compile_const_value(c, value.as.structt.fields[i], spec->fields[i].type);
        }

        LLVMValueRef result = LLVMConstStructInContext(c->llvm_context, fields, spec->fields_count, false);
        arena_reset(&temp_arena, fields);
        return result;
    }

    case CONST_VALUE_ARRAY: {
        const Const_Value_Array array = value.as.array;
        compile_type(c, array.element_type);

        LLVMValueRef memory = NULL;
        {
            const size_t group_values_count_save = c->group_values.count;
            for (size_t i = 0; i < array.count; i++) {
                da_push(&c->group_values, compile_const_value(c, array.data[i], *array.element_type));
            }
            LLVMValueRef *elements = &c->group_values.data[group_values_count_save];

            memory = LLVMConstArray(array.element_type->llvm, elements, array.count);
            c->group_values.count = group_values_count_save;
        }

        if (array.is_slice) {
            LLVMValueRef fields[] = {
                compile_const_value_into_memory(c, memory),
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), array.count, true),
            };
            memory = LLVMConstStructInContext(c->llvm_context, fields, len(fields), false);
        }
        return memory;
    }

    case CONST_VALUE_STRING:
        return compile_string_into_const_value(c, value.as.string);

    case CONST_VALUE_MODULE:
        unreachable();

    default:
        unreachable();
    }
}

static void compile_local_var_debug(Compiler *c, Node_Atom *it, LLVMMetadataRef var_debug_type) {
    const SV        name = it->node.token.sv;
    LLVMMetadataRef var_debug_metadata = NULL;
    if (it->definition_spec->arg_index) {
        var_debug_metadata = LLVMDIBuilderCreateParameterVariable(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            name.data,
            name.count,
            it->definition_spec->arg_index,
            get_debug_file(c, it->node.token.pos.path),
            it->node.token.pos.row + 1,
            var_debug_type,
            false,
            0);
    } else {
        var_debug_metadata = LLVMDIBuilderCreateAutoVariable(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            name.data,
            name.count,
            get_debug_file(c, it->node.token.pos.path),
            it->node.token.pos.row + 1,
            var_debug_type,
            false,
            0,
            LLVMABIAlignmentOfType(c->llvm_target_data, it->node.type.llvm));
    }

    LLVMMetadataRef var_pos_metadata = LLVMDIBuilderCreateDebugLocation(
        c->llvm_context, it->node.token.pos.row + 1, it->node.token.pos.col + 1, c->llvm_debug_scope, NULL);

    LLVMValueRef next_inst = c->llvm_fn_last_alloca ? LLVMGetNextInstruction(c->llvm_fn_last_alloca) : NULL;
    if (next_inst) {
        LLVMDIBuilderInsertDeclareRecordBefore(
            c->llvm_debug_builder,
            it->definition_spec->llvm,
            var_debug_metadata,
            LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
            var_pos_metadata,
            next_inst);
    } else {
        LLVMDIBuilderInsertDeclareRecordAtEnd(
            c->llvm_debug_builder,
            it->definition_spec->llvm,
            var_debug_metadata,
            LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
            var_pos_metadata,
            LLVMGetInsertBlock(c->llvm_builder));
    }
}

static void compile_var_def(Compiler *c, Node_Atom *it) {
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    compile_type(c, &it->node.type);

    SV link_as = {0};
    if (it->definition_spec->link_as.count) {
        // Guarantee a terminating '\0'
        link_as = sv_from_cstr(arena_sv_to_cstr(&temp_arena, it->definition_spec->link_as));
    }

    SV name = it->node.token.sv;
    if (it->definition_spec->is_extern) {
        // Guarantee a terminating '\0'
        name = sv_from_cstr(arena_sv_to_cstr(&temp_arena, name));
    } else if (it->definition_spec->static_var_fn) {
        const size_t start = default_sb.count;
        sb_push_nested_fn_name(c, &default_sb, it->definition_spec->static_var_fn, it->module);
        sb_sprintf(&default_sb, "." SV_Fmt, SV_Arg(name));
        name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
    } else if (!it->definition_spec->is_local) {
        name = sv_from_cstr(arena_sprintf(&temp_arena, SV_Fmt "." SV_Fmt, SV_Arg(it->module->name), SV_Arg(name)));
    }

    if (it->definition_spec->is_local && !it->definition_spec->is_extern && !it->definition_spec->static_var_fn) {
        it->definition_spec->llvm = compile_alloca(c, it->node.type.llvm);
    } else {
        if (!link_as.count) {
            link_as = name;
        }
        it->definition_spec->llvm = LLVMAddGlobal(c->llvm_module, it->node.type.llvm, link_as.data);
    }

    if (!it->definition_spec->is_extern) {
        LLVMMetadataRef var_debug_type = get_debug_for_type(c, &it->node.type);
        if (it->definition_spec->is_local && !it->definition_spec->static_var_fn) {
            if (!it->definition_spec->arg_index && !it->definition_spec->is_assigned) {
                LLVMBuildStore(c->llvm_builder, LLVMConstNull(it->node.type.llvm), it->definition_spec->llvm);
            }
            compile_local_var_debug(c, it, var_debug_type);
        } else {
            if (it->definition_spec->is_assigned) {
                LLVMSetInitializer(
                    it->definition_spec->llvm, compile_const_value(c, it->definition_spec->const_value, it->node.type));
            } else {
                LLVMSetInitializer(it->definition_spec->llvm, LLVMConstNull(it->node.type.llvm));
            }

            LLVMMetadataRef var_debug_metadata = LLVMDIBuilderCreateGlobalVariableExpression(
                c->llvm_debug_builder,
                get_debug_file(c, it->node.token.pos.path),
                name.data,
                name.count,
                link_as.data,
                link_as.count,
                get_debug_file(c, it->node.token.pos.path),
                it->node.token.pos.row + 1,
                var_debug_type,
                true,
                LLVMDIBuilderCreateExpression(c->llvm_debug_builder, NULL, 0),
                NULL,
                0);

            LLVMGlobalSetMetadata(it->definition_spec->llvm, 0, var_debug_metadata);
        }
    }

    arena_reset(&temp_arena, checkpoint);
}

static void compile_defers(Compiler *c, size_t from, bool rollback) {
    for (size_t i = c->defers.count; i > from; i--) {
        compile_stmt(c, c->defers.data[i - 1]);
    }

    if (rollback) {
        c->defers.count = from;
    }
}

typedef struct {
    size_t defers_start;

    LLVMValueRef llvm_fn;
    LLVMValueRef llvm_fn_last_alloca;

    LLVMMetadataRef   llvm_debug_scope;
    LLVMBasicBlockRef llvm_current_block;

    LLVMMetadataRef llvm_current_debug_location;
} Compile_Fn_Backup;

static void compile_fn_backup_save(Compiler *c, Compile_Fn_Backup *b) {
    b->defers_start = c->defers_start;
    c->defers_start = c->defers.count;

    b->llvm_fn = c->llvm_fn;
    b->llvm_fn_last_alloca = c->llvm_fn_last_alloca;

    b->llvm_debug_scope = c->llvm_debug_scope;
    b->llvm_current_block = LLVMGetInsertBlock(c->llvm_builder);

    b->llvm_current_debug_location = LLVMGetCurrentDebugLocation2(c->llvm_builder);
}

static void compile_fn_backup_restore(Compiler *c, const Compile_Fn_Backup *b) {
    c->defers.count = c->defers_start;
    c->defers_start = b->defers_start;

    c->llvm_fn = b->llvm_fn;
    c->llvm_fn_last_alloca = b->llvm_fn_last_alloca;

    c->llvm_debug_scope = b->llvm_debug_scope;
    LLVMPositionBuilderAtEnd(c->llvm_builder, b->llvm_current_block);
    LLVMSetCurrentDebugLocation2(c->llvm_builder, b->llvm_current_debug_location);
}

typedef struct {
    Type        *type;
    LLVMValueRef value;
} Typed_LLVM_Value;

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

static void         compile_call_begin(Compiler *c, Call_Compiler *call, Typed_LLVM_Value fn, size_t args_count);
static void         compile_call_arg(Compiler *c, Call_Compiler *call, size_t arg_index, Typed_LLVM_Value *arg);
static LLVMValueRef compile_call_finalize(Compiler *c, Call_Compiler *call, bool raw, bool ref);

static LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn) {
    if (fn->llvm) {
        return fn->llvm;
    }

    if (fn->wrapper) {
        compile_fn(c, fn->wrapper);
    }

    const void *checkpoint = arena_alloc(&temp_arena, 0);

    ABI abi = {0};
    abi.args = arena_alloc(&temp_arena, fn->args_count * sizeof(*abi.args));
    abi.args_count = fn->args_count;
    fn->node.type.llvm = compile_fn_type(c, fn->node.type, &abi);

    SV link_as = {0};
    if (fn->defined_as && fn->defined_as->definition_spec->link_as.count) {
        // Guarantee a terminating '\0'
        link_as = sv_from_cstr(arena_sv_to_cstr(&temp_arena, fn->defined_as->definition_spec->link_as));
    }

    if (fn->is_extern) {
        assert(fn->defined_as);
        if (!link_as.count) {
            link_as = fn->defined_as->node.token.sv;
        }
        fn->llvm = LLVMGetOrInsertFunction(c->llvm_module, link_as.data, link_as.count, fn->node.type.llvm);
    } else {
        Compile_Fn_Backup backup = {0};
        compile_fn_backup_save(c, &backup);

        SV fn_name = sv_from_cstr(temp_nested_fn_name(c, fn, fn->module));
        if (!link_as.count) {
            link_as = fn_name;
        }
        fn->llvm = LLVMAddFunction(c->llvm_module, link_as.data, fn->node.type.llvm);

        if (fn->is_inline) {
            LLVMAttributeRef alwaysinline = LLVMCreateEnumAttribute(c->llvm_context, c->llvm_attribute_alwaysinline, 0);
            LLVMAddAttributeAtIndex(fn->llvm, LLVMAttributeFunctionIndex, alwaysinline);
        }

        c->llvm_fn = fn->llvm;
        c->llvm_fn_last_alloca = NULL;

        LLVMMetadataRef fn_debug_type = NULL;
        const Type_Fn  *fn_type_spec = fn->node.type.spec.fn;

        {

            LLVMMetadataRef *arg_debug_types =
                arena_alloc(&temp_arena, (fn_type_spec->args_count + 1) * sizeof(*arg_debug_types));
            arg_debug_types[0] = get_debug_for_type(c, fn_type_spec->return_type);
            for (size_t i = 0; i < fn_type_spec->args_count; i++) {
                arg_debug_types[i + 1] = get_debug_for_type(c, &fn_type_spec->args[i].type);
            }

            fn_debug_type = LLVMDIBuilderCreateSubroutineType(
                c->llvm_debug_builder,
                get_debug_file(c, fn->node.token.pos.path),
                arg_debug_types,
                fn_type_spec->args_count + 1,
                0);

            arena_reset(&temp_arena, arg_debug_types);
        }

        fn->llvm_debug_scope = LLVMDIBuilderCreateFunction(
            c->llvm_debug_builder,
            get_scope_of_definition(c, (Node *) fn, fn->outer_fn),
            fn_name.data,
            fn_name.count,
            link_as.data,
            link_as.count,
            get_debug_file(c, fn->node.token.pos.path),
            fn->node.token.pos.row + 1,
            fn_debug_type,
            true,
            true,
            fn->node.token.pos.row + 1,
            0,
            false);

        LLVMSetSubprogram(fn->llvm, fn->llvm_debug_scope);
        c->llvm_debug_scope = fn->llvm_debug_scope;

        LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, fn->llvm, ""));
        LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);

        size_t abi_iota = 0;
        size_t arg_iota = 0;
        if (!abi.return_abi.direct_types_count) {
            arg_iota++;
            LLVMAttributeRef sret =
                LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, abi.return_type->llvm);
            LLVMAddAttributeAtIndex(fn->llvm, arg_iota, sret);
        }

        if (fn->wrapper) {
            set_debug_pos(c, fn->node.token.pos);

            Call_Compiler call = {0};
            compile_call_begin(
                c,
                &call,
                (Typed_LLVM_Value) {.value = fn->wrapper->llvm, .type = &fn->wrapper->node.type},
                fn->wrapper->args_count);

            assert(call.fn_spec->args_count);
            Typed_LLVM_Value receiver = {0};
            receiver.type = &call.fn_spec->args[0].type;
            receiver.value =
                LLVMBuildLoad2(c->llvm_builder, compile_type(c, receiver.type), LLVMGetParam(fn->llvm, arg_iota), "");

            compile_call_arg(c, &call, 0, &receiver);
            for (size_t i = 0; i < call.args_count; i++) {
                call.args_info[i] = get_abi_info_for_type(c, &call.fn_spec->args[i].type, true);
            }

            for (size_t i = 0; i < abi.args_count; i++) {
                const size_t direct_types_count = abi.args[i].direct_types_count;
                if (direct_types_count) {
                    arg_iota += direct_types_count;
                } else {
                    arg_iota++;

#ifdef PLATFORM_X86_64_LINUX
                    LLVMAttributeRef byval =
                        LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, abi.args[i].type);
                    LLVMAddAttributeAtIndex(fn->llvm, arg_iota, byval);
#endif // PLATFORM_X86_64_LINUX
                }
            }
            assert(arg_iota == abi.actual_args_count);

            const size_t actual_args_count = LLVMCountParams(call.fn.value);
            const size_t wrapper_args_count = LLVMCountParams(fn->llvm);
            const size_t actual_args_emitted = c->arg_values.count - call.arg_values_start;

            assert(actual_args_count >= wrapper_args_count);
            const size_t offset = actual_args_count - wrapper_args_count;

            for (size_t i = actual_args_emitted; i < actual_args_count; i++) {
                da_push(&c->arg_values, LLVMGetParam(fn->llvm, i - offset));
            }

            LLVMValueRef result = compile_call_finalize(c, &call, true, false);
            if (abi.return_type->kind == TYPE_UNIT) {
                LLVMBuildRetVoid(c->llvm_builder);
            } else if (abi.return_abi.direct_types_count == 0) {
                LLVMBuildStore(c->llvm_builder, result, LLVMGetParam(fn->llvm, 0));
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                LLVMBuildRet(c->llvm_builder, result);
            }
        } else {
            for (Node *arg = fn->args.head; arg; arg = arg->next) {
                assert(arg->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) arg;

                assert(define->name->kind == NODE_ATOM);
                Node_Atom *it = (Node_Atom *) define->name;
                assert(!it->definition_spec->llvm);

                const ABI_Info it_abi = abi.args[abi_iota++];

                static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
                switch (it_abi.direct_types_count) {
                case 0: {
                    it->definition_spec->llvm = LLVMGetParam(c->llvm_fn, arg_iota++);
                    compile_local_var_debug(c, it, get_debug_for_type(c, &it->node.type));

#ifdef PLATFORM_X86_64_LINUX
                    LLVMAttributeRef byval =
                        LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, it->node.type.llvm);
                    LLVMAddAttributeAtIndex(fn->llvm, arg_iota, byval);
#endif // PLATFORM_X86_64_LINUX
                } break;

                case 1: {
                    bool stored = false;

                    compile_var_def(c, it);
                    LLVMValueRef value = LLVMGetParam(c->llvm_fn, arg_iota++);
                    if (type_is_compound(it->node.type)) {
                        LLVMTypeRef  var_type = it->node.type.llvm;
                        const size_t var_size = LLVMABISizeOfType(c->llvm_target_data, var_type);
                        LLVMTypeRef  abi_type = LLVMTypeOf(value);
                        const size_t abi_size = LLVMABISizeOfType(c->llvm_target_data, abi_type);
                        if (abi_size > var_size) {
                            if (abi_size > 8) {
                                LLVMValueRef memory = compile_alloca(c, abi_type);
                                LLVMBuildStore(c->llvm_builder, value, memory);
                                LLVMBuildMemCpy(
                                    c->llvm_builder,
                                    it->definition_spec->llvm,
                                    LLVMABIAlignmentOfType(c->llvm_target_data, var_type),
                                    memory,
                                    LLVMABIAlignmentOfType(c->llvm_target_data, abi_type),
                                    LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), var_size, true));
                                stored = true;
                            } else {
                                value = LLVMBuildTrunc(
                                    c->llvm_builder, value, LLVMIntTypeInContext(c->llvm_context, var_size * 8), "");
                            }
                        }
                    }

                    if (!stored) {
                        LLVMBuildStore(c->llvm_builder, value, it->definition_spec->llvm);
                    }
                } break;

                case 2: {
                    compile_var_def(c, it);

                    // First half
                    LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), it->definition_spec->llvm);

                    // Second half
                    LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
                    LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
                    LLVMValueRef second = LLVMBuildGEP2(
                        c->llvm_builder, llvm_i8_type, it->definition_spec->llvm, indices, len(indices), "");
                    LLVMBuildStore(c->llvm_builder, LLVMGetParam(c->llvm_fn, arg_iota++), second);
                } break;

                default:
                    unreachable();
                }
            }
            assert(arg_iota == abi.actual_args_count);

            assert(fn->body->kind == NODE_BLOCK);
            Node_Block *block = (Node_Block *) fn->body;
            for (Node *it = block->body.head; it; it = it->next) {
                compile_stmt(c, it);
            }

            if (!fn_type_spec->returns_count) {
                compile_defers(c, c->defers_start, true);
                set_debug_pos(c, block->end);
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                // The semantic analyzer has already determined that the function returns in all execution paths.
                // No need to compile defers here, as this is unreachable.
                set_debug_pos(c, block->end);
                LLVMBuildUnreachable(c->llvm_builder);
            }
        }

        compile_fn_backup_restore(c, &backup);
    }

    arena_reset(&temp_arena, checkpoint);
    return fn->llvm;
}

static LLVMValueRef get_builtin_func(Compiler *c, SV name, LLVMTypeRef *type) {
    const Const_Value value = get_const_definition_value(c, c->builtin_module, name, NULL);
    assert(value.kind == CONST_VALUE_FN);

    LLVMValueRef fn = compile_fn(c, value.as.fn);
    if (type) {
        *type = value.as.fn->node.type.llvm;
    }
    return fn;
}

static void compile_panic(Compiler *c, const char *fmt, LLVMValueRef v1, LLVMValueRef v2, LLVMValueRef v3) {
    LLVMTypeRef  fn_type = NULL;
    LLVMValueRef fn_value = get_builtin_func(c, sv_from_cstr("panic_handler"), &fn_type);

    LLVMValueRef zero = LLVMConstNull(LLVMInt64TypeInContext(c->llvm_context));
    LLVMValueRef args[] = {
        LLVMBuildGlobalString(c->llvm_builder, fmt, ""),
        v1 ? v1 : zero,
        v2 ? v2 : zero,
        v3 ? v3 : zero,
    };

    LLVMBuildCall2(c->llvm_builder, fn_type, fn_value, args, len(args), "");
    LLVMBuildUnreachable(c->llvm_builder);
}

static LLVMValueRef compile_ident(Compiler *c, Node *n, Node_Atom *definition, bool ref) {
    assert(definition);
    if (definition->definition_spec->is_const) {
        const Const_Value const_value = definition->definition_spec->const_value;

        static_assert(COUNT_CONST_VALUES == 10, "");
        switch (const_value.kind) {
        case CONST_VALUE_TRAIT:
        case CONST_VALUE_UNION:
        case CONST_VALUE_STRUCT:
        case CONST_VALUE_ARRAY:
        case CONST_VALUE_STRING:
            if (!definition->definition_spec->llvm) {
                definition->definition_spec->llvm =
                    compile_const_value_into_memory(c, compile_const_value(c, const_value, n->type));
            }

            if (ref) {
                return definition->definition_spec->llvm;
            }

            set_debug_pos(c, n->token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");

        default:
            if (!definition->definition_spec->llvm) {
                definition->definition_spec->llvm = compile_const_value(c, const_value, n->type);
            }
            return definition->definition_spec->llvm;
        }
    }

    if (definition->is_ghost) {
        assert(definition->ghost_llvm);
        if (ref) {
            return definition->ghost_llvm;
        }

        set_debug_pos(c, n->token.pos);
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->ghost_llvm, "");
    }

    if (!definition->definition_spec->llvm) {
        compile_var_def(c, definition);
    }

    if (ref) {
        return definition->definition_spec->llvm;
    }

    set_debug_pos(c, n->token.pos);
    return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");
}

static uint64_t ht_hasheq_type(const void *va, const void *vb, size_t n) {
    unused(n);
    if (vb) {
        return type_eq(*(const Type *) va, *(const Type *) vb);
    }

    // Technically this is correct, however this will decay to O(n) very often.
    // TODO: Implement a more specific hashing algorithm for types
    uint64_t hash = 14695981039346656037UL;
    for (size_t i = 0; i < sizeof(Type); i++) {
        hash ^= *(const uint8_t *) va;
        hash *= 1099511628211UL;
    }
    return hash;
}

static LLVMValueRef
create_const_slice_from_memory(Compiler *c, LLVMTypeRef element, LLVMValueRef *memory, size_t count) {
    LLVMValueRef fields[2] = {0};
    if (count) {
        fields[0] = compile_const_value_into_memory(c, LLVMConstArray(element, memory, count));
    } else {
        fields[0] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
    }

    fields[1] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), count, true);
    return LLVMConstStructInContext(c->llvm_context, fields, len(fields), false);
}

static LLVMValueRef create_const_struct_from_single_value_if_not_already(Compiler *c, LLVMValueRef value) {
    if (LLVMGetValueKind(value) == LLVMConstantStructValueKind) {
        return value;
    }
    return LLVMConstStructInContext(c->llvm_context, &value, 1, false);
}

typedef struct {
    Type *type;

    size_t     variant_index;
    Type_Info *type_info;

    LLVMValueRef ti_fields[4];
    size_t       ti_fields_iota;

    LLVMValueRef tiv_fields[3];
    size_t       tiv_fields_iota;

    LLVMValueRef done;
} Type_Info_Compiler;

static void compile_type_info_init(Compiler *c, Type_Info_Compiler *tic, Type *type) {
    compile_type(c, type);
    tic->type = type;

    if (!c->type_info_cache.hasheq) {
        c->type_info_cache.hasheq = ht_hasheq_type;
    }

    tic->variant_index = c->type_info_variants[type->ref ? TYPE_RAWPTR : type->kind];
    assert(tic->variant_index);

    // Emit unique RTTI
    tic->type_info = NULL;
    {
        tic->type_info = ht_get(&c->type_info_cache, *type);
        if (tic->type_info) {
            tic->done = tic->type_info->info;
            return;
        }

        tic->type_info = ht_set(&c->type_info_cache, *type, (Type_Info) {0});
        tic->type_info->id = ++c->type_id_iota;
        tic->type_info->info = LLVMAddGlobal(c->llvm_module, c->type_info_type.llvm, "");
    }

    tic->ti_fields[tic->ti_fields_iota++] = LLVMConstInt(
        LLVMInt64TypeInContext(c->llvm_context), LLVMABISizeOfType(c->llvm_target_data, type->llvm), false);

    tic->ti_fields[tic->ti_fields_iota++] = LLVMConstInt(
        LLVMInt64TypeInContext(c->llvm_context), LLVMABIAlignmentOfType(c->llvm_target_data, type->llvm), false);

    tic->tiv_fields[tic->tiv_fields_iota++] =
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), tic->variant_index, false);
}

static void compile_type_info_fn(Compiler *c, Type_Info_Compiler *tic, bool skip_first_arg) {
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    LLVMValueRef fn_fields[2] = {0};
    size_t       fn_fields_iota = 0;

    assert(tic->type->kind == TYPE_FN);
    const Type_Fn *spec = tic->type->spec.fn;

    LLVMValueRef *args = arena_alloc(&temp_arena, spec->args_count * sizeof(*args));
    for (size_t i = skip_first_arg; i < spec->args_count; i++) {
        args[i] = compile_type_info(c, &spec->args[i].type);
    }
    fn_fields[fn_fields_iota++] = create_const_slice_from_memory(
        c, LLVMPointerTypeInContext(c->llvm_context, 0), args + skip_first_arg, spec->args_count - skip_first_arg);

    LLVMValueRef *returns = arena_alloc(&temp_arena, spec->returns_count * sizeof(*returns));
    for (size_t i = 0; i < spec->returns_count; i++) {
        returns[i] = compile_type_info(c, &spec->returns[i]);
    }
    fn_fields[fn_fields_iota++] =
        create_const_slice_from_memory(c, LLVMPointerTypeInContext(c->llvm_context, 0), returns, spec->returns_count);

    tic->tiv_fields[tic->tiv_fields_iota++] =
        LLVMConstStructInContext(c->llvm_context, fn_fields, fn_fields_iota, false);
    arena_reset(&temp_arena, checkpoint);
}

static LLVMValueRef compile_type_info_finalize(Compiler *c, Type_Info_Compiler *tic);

static void compile_type_info_variant(Compiler *c, Type_Info_Compiler *tic) {
    if (tic->done) {
        return;
    }

    if (tic->type->ref) {
        Type underlying = *tic->type;
        underlying.ref--;
        underlying.llvm = NULL;
        tic->tiv_fields[tic->tiv_fields_iota++] =
            create_const_struct_from_single_value_if_not_already(c, compile_type_info(c, &underlying));
    } else {
        switch (tic->type->kind) {
        case TYPE_BOOL:
        case TYPE_CHAR:
        case TYPE_RAWPTR:
        case TYPE_STRING:
            // Pass
            break;

        case TYPE_I8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_INT:

        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_struct_from_single_value_if_not_already(
                c, LLVMConstInt(LLVMInt1TypeInContext(c->llvm_context), type_is_signed(*tic->type), true));
            break;

        case TYPE_FN:
            compile_type_info_fn(c, tic, false);
            break;

        case TYPE_ENUM: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef enum_fields[3] = {0};
            size_t       enum_fields_iota = 0;

            const Type_Enum *spec = &tic->type->spec.enumm;

            LLVMValueRef *names = arena_alloc(&temp_arena, spec->definition->values_count * sizeof(*names));
            LLVMValueRef *values = arena_alloc(&temp_arena, spec->definition->values_count * sizeof(*values));

            {
                size_t iota = 0;
                ll_foreach(it, &spec->definition->values) {
                    names[iota] = compile_string_into_const_value(c, it->token.sv);
                    values[iota] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), it->token.as.integer, true);
                    iota++;
                }
                assert(iota == spec->definition->values_count);
            }

            enum_fields[enum_fields_iota++] =
                create_const_slice_from_memory(c, c->llvm_slice_type, names, spec->definition->values_count);

            enum_fields[enum_fields_iota++] = create_const_slice_from_memory(
                c, LLVMInt64TypeInContext(c->llvm_context), values, spec->definition->values_count);

            Type underlying = {.kind = spec->underlying};
            enum_fields[enum_fields_iota++] = compile_type_info(c, &underlying);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, enum_fields, enum_fields_iota, false);

            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_TRAIT: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef struct_fields[2] = {0};
            size_t       struct_fields_iota = 0;

            const Type_Trait *spec = tic->type->spec.trait;

            LLVMValueRef *names = arena_alloc(&temp_arena, spec->methods_count * sizeof(*names));
            LLVMValueRef *types = arena_alloc(&temp_arena, spec->methods_count * sizeof(*types));

            for (size_t i = 0; i < spec->methods_count; i++) {
                Type_Trait_Method *it = &spec->methods[i];
                names[i] = compile_string_into_const_value(c, it->name);

                Type_Info_Compiler tic = {0};
                compile_type_info_init(c, &tic, &it->type);
                compile_type_info_fn(c, &tic, true);
                types[i] = compile_type_info_finalize(c, &tic);
            }

            struct_fields[struct_fields_iota++] =
                create_const_slice_from_memory(c, c->llvm_slice_type, names, spec->methods_count);

            struct_fields[struct_fields_iota++] = create_const_slice_from_memory(
                c, LLVMPointerTypeInContext(c->llvm_context, 0), types, spec->methods_count);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, struct_fields, struct_fields_iota, false);
            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_UNION: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            const Type_Union *spec = tic->type->spec.unionn;

            LLVMValueRef *variants = arena_alloc(&temp_arena, spec->variants_count * sizeof(*variants));
            for (size_t i = 0; i < spec->variants_count; i++) {
                variants[i] = compile_type_info(c, &spec->variants[i].type);
            }
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_slice_from_memory(
                c, LLVMPointerTypeInContext(c->llvm_context, 0), variants, spec->variants_count);

            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_STRUCT: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef struct_fields[3] = {0};
            size_t       struct_fields_iota = 0;

            const Type_Struct *spec = tic->type->spec.structt;

            LLVMValueRef *names = arena_alloc(&temp_arena, spec->fields_count * sizeof(*names));
            LLVMValueRef *types = arena_alloc(&temp_arena, spec->fields_count * sizeof(*types));
            LLVMValueRef *offsets = arena_alloc(&temp_arena, spec->fields_count * sizeof(*offsets));

            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                names[i] = compile_string_into_const_value(c, it->name);
                types[i] = compile_type_info(c, &it->type);
                offsets[i] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), it->offset, true);
            }

            struct_fields[struct_fields_iota++] =
                create_const_slice_from_memory(c, c->llvm_slice_type, names, spec->fields_count);

            struct_fields[struct_fields_iota++] = create_const_slice_from_memory(
                c, LLVMPointerTypeInContext(c->llvm_context, 0), types, spec->fields_count);

            struct_fields[struct_fields_iota++] =
                create_const_slice_from_memory(c, LLVMInt64TypeInContext(c->llvm_context), offsets, spec->fields_count);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, struct_fields, struct_fields_iota, false);
            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_ARRAY: {
            LLVMValueRef array_fields[2] = {0};
            size_t       array_fields_iota = 0;

            array_fields[array_fields_iota++] = compile_type_info(c, tic->type->spec.array.element);
            array_fields[array_fields_iota++] =
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), tic->type->spec.array.count, true);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, array_fields, array_fields_iota, false);
        } break;

        case TYPE_SLICE:
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_struct_from_single_value_if_not_already(
                c, compile_type_info(c, tic->type->spec.slice.element));
            break;

        default:
            unreachable();
            break;
        }
    }
}

static LLVMValueRef compile_type_info_finalize(Compiler *c, Type_Info_Compiler *tic) {
    if (tic->done) {
        return tic->done;
    }

    const size_t variant_size = c->type_info_variants_union->variants[tic->variant_index - 1].size;
    const size_t variant_padding = c->type_info_variants_union->variants_size_max - variant_size;
    if (variant_padding) {
        tic->tiv_fields[tic->tiv_fields_iota++] =
            LLVMConstNull(LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), variant_padding));
    }

    tic->ti_fields[tic->ti_fields_iota++] =
        LLVMConstStructInContext(c->llvm_context, tic->tiv_fields, tic->tiv_fields_iota, false);
    tic->ti_fields[tic->ti_fields_iota++] =
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), tic->type_info->id, true);

    LLVMValueRef real = compile_const_value_into_memory(
        c, LLVMConstStructInContext(c->llvm_context, tic->ti_fields, tic->ti_fields_iota, false));

    LLVMReplaceAllUsesWith(tic->type_info->info, real);
    LLVMDeleteGlobal(tic->type_info->info);
    tic->type_info->info = real;
    tic->done = real;
    return real;
}

static_assert(COUNT_TYPES == 25, "");
static LLVMValueRef compile_type_info(Compiler *c, Type *type) {
    Type_Info_Compiler tic = {0};
    compile_type_info_init(c, &tic, type);
    compile_type_info_variant(c, &tic);
    return compile_type_info_finalize(c, &tic);
}

static LLVMValueRef
compile_cast_to_trait(Compiler *c, Type *type, Type_Trait_Impl *impl, LLVMValueRef value, bool ref) //
{
    compile_trait_impl(c, impl);

    LLVMValueRef value_memory = compile_alloca(c, LLVMTypeOf(value));
    LLVMBuildStore(c->llvm_builder, value, value_memory);

    LLVMValueRef trait_memory = compile_alloca(c, c->llvm_trait_type);
    LLVMBuildStore(c->llvm_builder, compile_type_info(c, type), trait_memory);
    LLVMBuildStore(
        c->llvm_builder, value_memory, LLVMBuildStructGEP2(c->llvm_builder, c->llvm_trait_type, trait_memory, 1, ""));
    LLVMBuildStore(
        c->llvm_builder, impl->llvm, LLVMBuildStructGEP2(c->llvm_builder, c->llvm_trait_type, trait_memory, 2, ""));

    if (ref) {
        return trait_memory;
    }
    return LLVMBuildLoad2(c->llvm_builder, c->llvm_trait_type, trait_memory, "");
}

static LLVMValueRef
compile_cast_to_union(Compiler *c, LLVMTypeRef union_type, size_t union_index, LLVMValueRef from, bool ref) //
{
    LLVMValueRef memory = compile_alloca(c, union_type);
    LLVMBuildStore(c->llvm_builder, LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), union_index, true), memory);

    LLVMValueRef payload = LLVMBuildStructGEP2(c->llvm_builder, union_type, memory, 1, "");
    LLVMBuildStore(c->llvm_builder, from, payload);

    if (ref) {
        return memory;
    }
    return LLVMBuildLoad2(c->llvm_builder, union_type, memory, "");
}

static LLVMValueRef compile_load_if_not_null(Compiler *c, LLVMValueRef ptr, LLVMTypeRef type) {
    LLVMValueRef is_null =
        LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, ptr, LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0)), "");

    LLVMBasicBlockRef null_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBasicBlockRef not_null_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBuildCondBr(c->llvm_builder, is_null, null_block, not_null_block);

    LLVMPositionBuilderAtEnd(c->llvm_builder, null_block);
    LLVMValueRef null_value = LLVMConstNull(type);
    LLVMBuildBr(c->llvm_builder, merge_block);

    LLVMPositionBuilderAtEnd(c->llvm_builder, not_null_block);
    LLVMValueRef not_null_value = LLVMBuildLoad2(c->llvm_builder, type, ptr, "");
    LLVMBuildBr(c->llvm_builder, merge_block);

    LLVMPositionBuilderAtEnd(c->llvm_builder, merge_block);
    LLVMValueRef      phi = LLVMBuildPhi(c->llvm_builder, type, "");
    LLVMValueRef      phi_values[] = {null_value, not_null_value};
    LLVMBasicBlockRef phi_blocks[] = {null_block, not_null_block};
    LLVMAddIncoming(phi, phi_values, phi_blocks, len(phi_blocks));
    return phi;
}

static void compile_call_begin(Compiler *c, Call_Compiler *call, Typed_LLVM_Value fn, size_t args_count) {
    call->checkpoint = arena_alloc(&temp_arena, 0);
    call->debug_pos = LLVMGetCurrentDebugLocation2(c->llvm_builder);

    assert(fn.type->kind == TYPE_FN);
    call->fn_spec = fn.type->spec.fn;
    if (!call->fn_spec->llvm) {
        ABI abi = {0};
        abi.args_count = call->fn_spec->args_count;
        abi.args = arena_alloc(&temp_arena, abi.args_count * sizeof(*abi.args));
        compile_fn_type(c, *fn.type, &abi);
    }
    fn.type->llvm = call->fn_spec->llvm;
    call->arg_values_start = c->arg_values.count;

    call->return_info = get_abi_info_for_type(c, call->fn_spec->return_type, false);
    if (!call->return_info.direct_types_count) {
        da_push(&c->arg_values, compile_alloca(c, call->fn_spec->return_type->llvm));
    }
    call->args_info = arena_alloc(&temp_arena, args_count * sizeof(*call->args_info));
    call->fn = fn;
    call->args_count = args_count;
}

static void compile_call_arg(Compiler *c, Call_Compiler *call, size_t arg_index, Typed_LLVM_Value *arg) {
    LLVMValueRef expr = arg->value;

    const ABI_Info arg_info = get_abi_info_for_type(c, arg->type, true);
    call->args_info[arg_index] = arg_info;

    static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
    switch (arg_info.direct_types_count) {
    case 0: {
#ifdef PLATFORM_X86_64_LINUX
        expr = undo_load(expr);
#else
        LLVMValueRef temp = compile_alloca(c, arg->type->llvm);
        LLVMBuildStore(c->llvm_builder, expr, temp);
        expr = temp;
#endif // PLATFORM_X86_64_LINUX

        da_push(&c->arg_values, expr);
    } break;

    case 1: {
        if (type_is_compound(*arg->type)) {
            LLVMTypeRef  abi_type = arg_info.direct_types[0];
            const size_t abi_size = LLVMABISizeOfType(c->llvm_target_data, abi_type);
            LLVMTypeRef  expr_type = LLVMTypeOf(expr);
            const size_t expr_size = LLVMABISizeOfType(c->llvm_target_data, expr_type);

            expr = undo_load(expr);
            if (abi_size > expr_size) {
                if (abi_size > 8) {
                    LLVMValueRef memory = compile_alloca(c, abi_type);
                    LLVMBuildMemCpy(
                        c->llvm_builder,
                        memory,
                        LLVMABIAlignmentOfType(c->llvm_target_data, abi_type),
                        expr,
                        LLVMABIAlignmentOfType(c->llvm_target_data, expr_type),
                        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), expr_size, true));
                    expr = LLVMBuildLoad2(c->llvm_builder, abi_type, memory, "");
                } else {
                    expr =
                        LLVMBuildLoad2(c->llvm_builder, LLVMIntTypeInContext(c->llvm_context, expr_size * 8), expr, "");
                    expr = LLVMBuildZExt(c->llvm_builder, expr, abi_type, "");
                }
            } else {
                expr = LLVMBuildLoad2(c->llvm_builder, abi_type, expr, "");
            }
        } else {
            if (call->fn_spec->variadics_kind == VARIADICS_UNTYPED) {
                const size_t size = compile_sizeof(c, arg->type);
                if (size < 4) {
                    // Promote values smaller than i32 into i32
                    expr = compile_cast(c, expr, LLVMInt32TypeInContext(c->llvm_context), type_is_signed(*arg->type));
                }
            }
        }
        da_push(&c->arg_values, expr);
    } break;

    case 2: {
        // First half
        LLVMValueRef first = undo_load(expr);
        da_push(&c->arg_values, LLVMBuildLoad2(c->llvm_builder, arg_info.direct_types[0], first, ""));

        // Second half
        LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
        LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
        LLVMValueRef second = LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, first, indices, len(indices), "");
        da_push(&c->arg_values, LLVMBuildLoad2(c->llvm_builder, arg_info.direct_types[1], second, ""));
    } break;

    default:
        unreachable();
    }
}

static LLVMValueRef compile_call_finalize(Compiler *c, Call_Compiler *call, bool raw, bool ref) {
    LLVMSetCurrentDebugLocation2(c->llvm_builder, call->debug_pos);
    LLVMValueRef result = LLVMBuildCall2(
        c->llvm_builder,
        call->fn.type->llvm,
        call->fn.value,
        &c->arg_values.data[call->arg_values_start],
        c->arg_values.count - call->arg_values_start,
        "");

    LLVMValueRef memory = NULL;

    size_t args_iota = 0;
    if (type_is_compound(*call->fn_spec->return_type)) {
        static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
        switch (call->return_info.direct_types_count) {
        case 0: {
            memory = c->arg_values.data[call->arg_values_start + args_iota++];
            LLVMAttributeRef sret =
                LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_sret, call->fn_spec->return_type->llvm);
            LLVMAddCallSiteAttribute(result, args_iota, sret);
        } break;

        case 1:
            if (!raw) {
                memory = compile_alloca(c, call->fn_spec->return_type->llvm);

                LLVMTypeRef  abi_type = call->return_info.direct_types[0];
                const size_t abi_size = LLVMABISizeOfType(c->llvm_target_data, abi_type);

                LLVMTypeRef  expr_type = call->fn_spec->return_type->llvm;
                const size_t expr_size = LLVMABISizeOfType(c->llvm_target_data, expr_type);
                if (abi_size > expr_size) {
                    LLVMValueRef expr = compile_alloca(c, abi_type);
                    LLVMBuildStore(c->llvm_builder, result, expr);
                    LLVMBuildMemCpy(
                        c->llvm_builder,
                        memory,
                        LLVMABIAlignmentOfType(c->llvm_target_data, abi_type),
                        expr,
                        LLVMABIAlignmentOfType(c->llvm_target_data, expr_type),
                        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), expr_size, true));
                } else {
                    LLVMBuildStore(c->llvm_builder, result, memory);
                }
            }
            break;

        case 2:
            if (!raw) {
                memory = compile_alloca(c, call->fn_spec->return_type->llvm);

                // First half
                LLVMValueRef first = memory;
                LLVMBuildStore(c->llvm_builder, LLVMBuildExtractValue(c->llvm_builder, result, 0, ""), first);

                // Second half
                LLVMTypeRef  llvm_i8_type = LLVMInt8TypeInContext(c->llvm_context);
                LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 8, false)};
                LLVMValueRef second = LLVMBuildGEP2(c->llvm_builder, llvm_i8_type, memory, indices, len(indices), "");
                LLVMBuildStore(c->llvm_builder, LLVMBuildExtractValue(c->llvm_builder, result, 1, ""), second);
            }
            break;

        default:
            unreachable();
        }
    }

#ifdef PLATFORM_X86_64_LINUX
    for (size_t i = 0; i < call->args_count; i++) {
        const ABI_Info it_abi = call->args_info[i];
        if (it_abi.direct_types_count) {
            args_iota += it_abi.direct_types_count;
        } else {
            args_iota++;

            LLVMTypeRef it_type = it_abi.type;
            assert(it_type);

            LLVMAttributeRef byval = LLVMCreateTypeAttribute(c->llvm_context, c->llvm_attribute_byval, it_type);
            LLVMAddCallSiteAttribute(result, args_iota, byval);
        }
    }
    assert(args_iota == c->arg_values.count - call->arg_values_start);
#endif // PLATFORM_X86_64_LINUX

    c->arg_values.count = call->arg_values_start;
    arena_reset(&temp_arena, call->checkpoint);

    if (memory) {
        if (ref) {
            return memory;
        }

        return LLVMBuildLoad2(c->llvm_builder, call->fn_spec->return_type->llvm, memory, "");
    }
    return result;
}

static LLVMValueRef compile_call(
    Compiler *c, Typed_LLVM_Value fn, Typed_LLVM_Value *args, size_t args_count, bool is_trait_call, bool ref) //
{
    Call_Compiler call = {0};
    compile_call_begin(c, &call, fn, args_count);
    for (size_t i = 0; i < args_count; i++) {
        if (i == 0 && is_trait_call) {
            Type             rawptr = {.kind = TYPE_RAWPTR};
            Typed_LLVM_Value receiver = {0};
            receiver.type = &rawptr;
            receiver.value = args[i].value;
            compile_call_arg(c, &call, i, &receiver);
            continue;
        }

        compile_call_arg(c, &call, i, &args[i]);
    }
    return compile_call_finalize(c, &call, false, ref);
}

static void
compile_optional_arguments(Compiler *c, Typed_LLVM_Value *args, const Type_Fn *fn_spec, Pos caller_location) {
    LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);

    for (size_t i = 0; i < fn_spec->args_count; i++) {
        if (args[i].value) {
            continue;
        }

        Type_Fn_Arg *arg = &fn_spec->args[i];
        compile_type(c, &arg->type);

        LLVMValueRef value = NULL;
        if (arg->default_value_is_caller_location) {
            LLVMValueRef memory = compile_alloca(c, arg->type.llvm);
            LLVMBuildStore(
                c->llvm_builder, compile_string_into_const_value(c, sv_from_cstr(caller_location.path)), memory);

            LLVMBuildStore(
                c->llvm_builder,
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), caller_location.row + 1, true),
                LLVMBuildStructGEP2(c->llvm_builder, arg->type.llvm, memory, 1, ""));

            LLVMBuildStore(
                c->llvm_builder,
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), caller_location.col + 1, true),
                LLVMBuildStructGEP2(c->llvm_builder, arg->type.llvm, memory, 2, ""));

            value = LLVMBuildLoad2(c->llvm_builder, arg->type.llvm, memory, "");
        } else {
            static_assert(COUNT_CONST_VALUES == 10, "");
            switch (arg->default_value->kind) {
            case CONST_VALUE_TRAIT:
            case CONST_VALUE_UNION:
            case CONST_VALUE_STRUCT:
            case CONST_VALUE_ARRAY:
            case CONST_VALUE_STRING:
                if (!arg->default_value_llvm) {
                    arg->default_value_llvm =
                        compile_const_value_into_memory(c, compile_const_value(c, *arg->default_value, arg->type));
                }

                value = LLVMBuildLoad2(c->llvm_builder, arg->type.llvm, arg->default_value_llvm, "");
                break;

            default:
                if (!arg->default_value_llvm) {
                    arg->default_value_llvm = compile_const_value(c, *arg->default_value, arg->type);
                }

                value = arg->default_value_llvm;
                break;
            }
        }

        Typed_LLVM_Value tv = {0};
        tv.type = &arg->type;
        tv.value = value;
        args[i] = tv;
    }

    set_debug_pos(c, caller_location);
}

static LLVMValueRef compile_binary_with_overloaded_operator(
    Compiler *c, Node_Binary *binary, size_t index, LLVMValueRef lhs, LLVMValueRef rhs) //
{
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    Node_Fn *overload = binary->overloads ? binary->overloads[index] : binary->overload;

    Typed_LLVM_Value fn = {0};
    fn.value = compile_fn(c, overload);
    fn.type = &overload->node.type;

    const Type_Fn    *fn_spec = fn.type->spec.fn;
    Typed_LLVM_Value *args = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*args));
    if (fn_spec->args[0].type.ref > binary->lhs->type.ref) {
        lhs = undo_load(lhs);
    }

    args[0].value = lhs;
    args[0].type = &fn_spec->args[0].type;

    args[1].value = rhs;
    args[1].type = &fn_spec->args[1].type;

    compile_optional_arguments(c, args, fn_spec, binary->node.token.pos);
    LLVMValueRef result = compile_call(c, fn, args, fn_spec->args_count, false, false);

    arena_reset(&temp_arena, checkpoint);
    return result;
}

static_assert(COUNT_NODES == 28, "");
static LLVMValueRef compile_expr_impl(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    if (n->type.kind == TYPE_MODULE) {
        return NULL;
    }

    if (n->type.kind != TYPE_GROUP) {
        compile_type(c, &n->type);
    }

    if (n->emit_type_info) {
        return compile_type_info(c, n->emit_type_info);
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 77, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            return LLVMConstInt(n->type.llvm, n->token.as.integer, type_is_signed(n->type));

        case TOKEN_NULL:
            return LLVMConstNull(n->type.llvm);

        case TOKEN_IDENT:
            return compile_ident(c, n, (Node_Atom *) atom->definition, ref);

        case TOKEN_STRING:
        case TOKEN_ISTRING: {
            LLVMValueRef memory = compile_const_value_into_memory(c, compile_string_into_const_value(c, n->token.sv));
            if (ref) {
                return memory;
            }

            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
        }

        case TOKEN_DIRECTIVE_MAIN:
            return compile_fn(c, c->main_fn);

        case TOKEN_DIRECTIVE_PLATFORM:
            return compile_const_value(c, get_platform(c, NULL), n->type);

        default:
            unreachable();
        }
    }

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;
        ll_foreach(it, &group->nodes) {
            LLVMValueRef value = compile_expr(c, it, ref);
            if (it->type.kind != TYPE_GROUP) {
                da_push(&c->group_values, value);
            }
        }
        return NULL;
    }

    case NODE_UNARY: {
        Node_Unary  *unary = (Node_Unary *) n;
        LLVMValueRef value = NULL;

        static_assert(COUNT_TOKENS == 77, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            value = compile_expr(c, unary->value, false);
            set_debug_pos(c, n->token.pos);

            if (unary->overload) {
                const void *checkpoint = arena_alloc(&temp_arena, 0);

                Typed_LLVM_Value fn = {0};
                fn.value = compile_fn(c, unary->overload);
                fn.type = &unary->overload->node.type;

                const Type_Fn    *fn_spec = fn.type->spec.fn;
                Typed_LLVM_Value *args = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*args));
                if (fn_spec->args[0].type.ref > unary->value->type.ref) {
                    value = undo_load(value);
                }

                args[0].value = value;
                args[0].type = &fn_spec->args[0].type;

                compile_optional_arguments(c, args, fn_spec, n->token.pos);
                LLVMValueRef result = compile_call(c, fn, args, fn_spec->args_count, false, false);

                arena_reset(&temp_arena, checkpoint);
                return result;
            }

            return LLVMBuildNeg(c->llvm_builder, value, "");

        case TOKEN_MUL:
            value = compile_expr(c, unary->value, false);
            if (ref) {
                return value;
            }

            set_debug_pos(c, n->token.pos);
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, value, "");

        case TOKEN_BAND:
            return compile_expr(c, unary->value, true);

        case TOKEN_BNOT:
            value = compile_expr(c, unary->value, false);
            set_debug_pos(c, n->token.pos);
            return LLVMBuildNot(c->llvm_builder, value, "");

        case TOKEN_LNOT:
            value = compile_expr(c, unary->value, false);
            set_debug_pos(c, n->token.pos);
            return LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, value, LLVMConstNull(n->type.llvm), "");

        case TOKEN_SIZEOF:
            return LLVMConstInt(n->type.llvm, compile_sizeof(c, &unary->value->type), false);

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;

        if (binary->trait_check) {
            LLVMTypeRef  ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);
            LLVMValueRef value =
                LLVMBuildLoad2(c->llvm_builder, ptr_type, compile_expr(c, binary->trait_check, true), "");

            LLVMValueRef expected = NULL;
            if (binary->trait_check_type) {
                expected = compile_type_info(c, binary->trait_check_type);
            } else {
                expected = LLVMConstNull(ptr_type);
            }

            return LLVMBuildICmp(
                c->llvm_builder, n->token.kind == TOKEN_EQ ? LLVMIntEQ : LLVMIntNE, value, expected, "");
        }

        if (binary->union_check) {
            LLVMTypeRef  i64_type = LLVMInt64TypeInContext(c->llvm_context);
            LLVMValueRef value =
                LLVMBuildLoad2(c->llvm_builder, i64_type, compile_expr(c, binary->union_check, true), "");

            return LLVMBuildICmp(
                c->llvm_builder,
                n->token.kind == TOKEN_EQ ? LLVMIntEQ : LLVMIntNE,
                value,
                LLVMConstInt(i64_type, binary->union_check_index, true),
                "");
        }

        // Arithmetic
        {
            typedef struct {
                LLVMValueRef (*i)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
                LLVMValueRef (*u)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
            } Op;

            static_assert(COUNT_TOKENS == 77, "");
            static const Op ops[COUNT_TOKENS] = {
                [TOKEN_ADD] = {.i = LLVMBuildAdd},
                [TOKEN_SUB] = {.i = LLVMBuildSub},
                [TOKEN_MUL] = {.i = LLVMBuildMul},
                [TOKEN_DIV] = {.i = LLVMBuildSDiv, .u = LLVMBuildUDiv},
                [TOKEN_MOD] = {.i = LLVMBuildSRem, .u = LLVMBuildURem},

                [TOKEN_SHL] = {.i = LLVMBuildShl},
                [TOKEN_SHR] = {.i = LLVMBuildAShr, .u = LLVMBuildLShr},
                [TOKEN_BOR] = {.i = LLVMBuildOr},
                [TOKEN_BAND] = {.i = LLVMBuildAnd},
            };

            const Op op = ops[n->token.kind];
            if (op.i) {
                LLVMValueRef lhs = compile_expr(c, binary->lhs, false);
                LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
                LLVMValueRef result = NULL;

                const bool is_pointer_arithmetic = type_is_pointer(n->type);
                if (is_pointer_arithmetic) {
                    LLVMTypeRef llvm_type_i64 = LLVMInt64TypeInContext(c->llvm_context);
                    lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                    rhs = LLVMBuildPtrToInt(c->llvm_builder, rhs, llvm_type_i64, "");
                }

                set_debug_pos(c, n->token.pos);
                if (binary->overload) {
                    result = compile_binary_with_overloaded_operator(c, binary, 0, lhs, rhs);
                } else if (op.u && !type_is_signed(binary->lhs->type)) {
                    result = op.u(c->llvm_builder, lhs, rhs, "");
                } else {
                    result = op.i(c->llvm_builder, lhs, rhs, "");
                }

                if (is_pointer_arithmetic) {
                    result = LLVMBuildIntToPtr(c->llvm_builder, result, n->type.llvm, "");
                }
                return result;
            }
        }

        // Comparison
        {
            typedef struct {
                LLVMIntPredicate i;
                LLVMIntPredicate u;
            } Op;

            static_assert(COUNT_TOKENS == 77, "");
            static const Op ops[COUNT_TOKENS] = {
                [TOKEN_GT] = {.i = LLVMIntSGT, .u = LLVMIntUGT},
                [TOKEN_GE] = {.i = LLVMIntSGE, .u = LLVMIntUGE},
                [TOKEN_LT] = {.i = LLVMIntSLT, .u = LLVMIntULT},
                [TOKEN_LE] = {.i = LLVMIntSLE, .u = LLVMIntULE},
                [TOKEN_EQ] = {.i = LLVMIntEQ},
                [TOKEN_NE] = {.i = LLVMIntNE},
            };

            const Op op = ops[n->token.kind];
            if (op.i) {
                LLVMValueRef lhs = compile_expr(c, binary->lhs, false);
                LLVMValueRef rhs = compile_expr(c, binary->rhs, false);

                set_debug_pos(c, n->token.pos);
                if (binary->overload) {
                    LLVMValueRef value = compile_binary_with_overloaded_operator(c, binary, 0, lhs, rhs);
                    if (binary->overload->is_compare_operator_complete) {
                        return LLVMBuildICmp(c->llvm_builder, op.i, value, LLVMConstNull(LLVMTypeOf(value)), "");
                    }

                    if (n->token.kind == TOKEN_EQ) {
                        return value;
                    } else if (n->token.kind == TOKEN_NE) {
                        return LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, value, LLVMConstNull(LLVMTypeOf(value)), "");
                    }
                } else if (op.u && !type_is_signed(binary->lhs->type)) {
                    return LLVMBuildICmp(c->llvm_builder, op.u, lhs, rhs, "");
                } else {
                    return LLVMBuildICmp(c->llvm_builder, op.i, lhs, rhs, "");
                }
            }
        }

        // Arithmetic assignment
        {
            typedef struct {
                LLVMValueRef (*i)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
                LLVMValueRef (*u)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);
            } Op;

            static_assert(COUNT_TOKENS == 77, "");
            static const Op ops[COUNT_TOKENS] = {
                [TOKEN_ADD_SET] = {.i = LLVMBuildAdd},
                [TOKEN_SUB_SET] = {.i = LLVMBuildSub},
                [TOKEN_MUL_SET] = {.i = LLVMBuildMul},
                [TOKEN_DIV_SET] = {.i = LLVMBuildSDiv, .u = LLVMBuildUDiv},
                [TOKEN_MOD_SET] = {.i = LLVMBuildSRem, .u = LLVMBuildURem},

                [TOKEN_SHL_SET] = {.i = LLVMBuildShl},
                [TOKEN_SHR_SET] = {.i = LLVMBuildAShr, .u = LLVMBuildLShr},
                [TOKEN_BOR_SET] = {.i = LLVMBuildOr},
                [TOKEN_BAND_SET] = {.i = LLVMBuildAnd},
            };

            const Op op = ops[n->token.kind];
            if (op.i) {
                const size_t group_values_count_save = c->group_values.count;
                const size_t group_count =
                    binary->lhs->type.kind == TYPE_GROUP ? binary->lhs->type.spec.group.count : 0;

                LLVMTypeRef llvm_type_i64 = LLVMInt64TypeInContext(c->llvm_context);
                LLVMTypeRef llvm_type_ptr = LLVMPointerTypeInContext(c->llvm_context, 0);

                const size_t group_values_ptr_start = c->group_values.count;
                LLVMValueRef ptr = compile_expr(c, binary->lhs, true);

                const size_t group_values_lhs_start = c->group_values.count;
                LLVMValueRef lhs = NULL;
                if (group_count) {
                    assert(c->group_values.count == group_values_count_save + group_count);
                    for (size_t i = 0; i < group_count; i++) {
                        LLVMValueRef ptr = c->group_values.data[group_values_ptr_start + i];

                        Type *type = &binary->lhs->type.spec.group.data[i];
                        compile_type(c, type);

                        LLVMValueRef lhs = LLVMBuildLoad2(c->llvm_builder, type->llvm, ptr, "");
                        if (type_is_pointer(*type)) {
                            lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                        }
                        da_push(&c->group_values, lhs);
                    }
                    assert(c->group_values.count == group_values_count_save + group_count * 2);
                } else {
                    lhs = LLVMBuildLoad2(c->llvm_builder, binary->lhs->type.llvm, ptr, "");
                    if (type_is_pointer(binary->lhs->type)) {
                        lhs = LLVMBuildPtrToInt(c->llvm_builder, lhs, llvm_type_i64, "");
                    }
                }

                const size_t group_values_rhs_start = c->group_values.count;
                LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
                if (group_count) {
                    assert(c->group_values.count == group_values_count_save + group_count * 3);
                    for (size_t i = 0; i < group_count; i++) {
                        LLVMValueRef *rhs = &c->group_values.data[group_values_rhs_start + i];
                        if (type_is_pointer(binary->lhs->type.spec.group.data[i])) {
                            *rhs = LLVMBuildPtrToInt(c->llvm_builder, *rhs, llvm_type_i64, "");
                        }
                    }
                } else {
                    if (type_is_pointer(binary->lhs->type)) {
                        rhs = LLVMBuildPtrToInt(c->llvm_builder, rhs, llvm_type_i64, "");
                    }
                }

                set_debug_pos(c, n->token.pos);
                if (group_count) {
                    assert(c->group_values.count == group_values_count_save + group_count * 3);
                    for (size_t i = 0; i < group_count; i++) {
                        LLVMValueRef ptr = c->group_values.data[group_values_ptr_start + i];
                        LLVMValueRef lhs = c->group_values.data[group_values_lhs_start + i];
                        LLVMValueRef rhs = c->group_values.data[group_values_rhs_start + i];

                        LLVMValueRef result = NULL;
                        if (binary->overloads[i]) {
                            result = compile_binary_with_overloaded_operator(c, binary, i, lhs, rhs);
                        } else if (op.u && !type_is_signed(binary->lhs->type)) {
                            result = op.u(c->llvm_builder, lhs, rhs, "");
                        } else {
                            result = op.i(c->llvm_builder, lhs, rhs, "");
                        }

                        if (type_is_pointer(binary->lhs->type.spec.group.data[i])) {
                            result = LLVMBuildIntToPtr(c->llvm_builder, result, llvm_type_ptr, "");
                        }
                        LLVMBuildStore(c->llvm_builder, result, ptr);
                    }
                } else {
                    LLVMValueRef result = NULL;
                    if (binary->overload) {
                        result = compile_binary_with_overloaded_operator(c, binary, 0, lhs, rhs);
                    } else if (op.u && !type_is_signed(binary->lhs->type)) {
                        result = op.u(c->llvm_builder, lhs, rhs, "");
                    } else {
                        result = op.i(c->llvm_builder, lhs, rhs, "");
                    }

                    if (type_is_pointer(binary->lhs->type)) {
                        result = LLVMBuildIntToPtr(c->llvm_builder, result, llvm_type_ptr, "");
                    }
                    LLVMBuildStore(c->llvm_builder, result, ptr);
                }

                c->group_values.count = group_values_count_save;
                return NULL;
            }
        }

        static_assert(COUNT_TOKENS == 77, "");
        switch (n->token.kind) {
        case TOKEN_SET: {
            const size_t group_values_count_save = c->group_values.count;

            LLVMValueRef lhs = compile_expr(c, binary->lhs, true);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
            set_debug_pos(c, n->token.pos);

            if (binary->lhs->type.kind == TYPE_GROUP) {
                const size_t count = binary->lhs->type.spec.group.count;
                assert(c->group_values.count == group_values_count_save + count * 2);
                for (size_t i = 0; i < count; i++) {
                    LLVMValueRef ptr = c->group_values.data[group_values_count_save + i];
                    LLVMValueRef value = c->group_values.data[group_values_count_save + count + i];
                    LLVMBuildStore(c->llvm_builder, value, ptr);
                }
            } else {
                LLVMBuildStore(c->llvm_builder, rhs, lhs);
            }

            c->group_values.count = group_values_count_save;
            return NULL;
        }

        case TOKEN_LOR: {
            LLVMValueRef lhs = compile_expr(c, binary->lhs, false);

            LLVMBasicBlockRef true_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef false_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBuildCondBr(c->llvm_builder, lhs, true_block, false_block);

            // Short circuit if lhs is true
            LLVMPositionBuilderAtEnd(c->llvm_builder, true_block);
            LLVMBuildBr(c->llvm_builder, merge_block);

            // Check rhs if lhs is false
            LLVMPositionBuilderAtEnd(c->llvm_builder, false_block);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
            LLVMBuildBr(c->llvm_builder, merge_block);

            // Merge
            LLVMPositionBuilderAtEnd(c->llvm_builder, merge_block);
            LLVMValueRef      phi = LLVMBuildPhi(c->llvm_builder, n->type.llvm, "");
            LLVMValueRef      phi_values[] = {lhs, rhs};
            LLVMBasicBlockRef phi_blocks[] = {true_block, false_block};
            LLVMAddIncoming(phi, phi_values, phi_blocks, len(phi_blocks));
            return phi;
        }

        case TOKEN_LAND: {
            LLVMValueRef lhs = compile_expr(c, binary->lhs, false);

            LLVMBasicBlockRef true_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef false_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBuildCondBr(c->llvm_builder, lhs, true_block, false_block);

            // Short circuit if lhs is false
            LLVMPositionBuilderAtEnd(c->llvm_builder, false_block);
            LLVMBuildBr(c->llvm_builder, merge_block);

            // Check rhs if lhs is true
            LLVMPositionBuilderAtEnd(c->llvm_builder, true_block);
            LLVMValueRef rhs = compile_expr(c, binary->rhs, false);
            LLVMBuildBr(c->llvm_builder, merge_block);

            // Merge
            LLVMPositionBuilderAtEnd(c->llvm_builder, merge_block);
            LLVMValueRef      phi = LLVMBuildPhi(c->llvm_builder, n->type.llvm, "");
            LLVMValueRef      phi_values[] = {lhs, rhs};
            LLVMBasicBlockRef phi_blocks[] = {false_block, true_block};
            LLVMAddIncoming(phi, phi_values, phi_blocks, len(phi_blocks));
            return phi;
        }

        default:
            unreachable();
        }
    }

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        if (member->is_enum) {
            return LLVMConstInt(n->type.llvm, member->enum_value, type_is_signed(n->type));
        }

        if (member->lhs->type.kind == TYPE_MODULE) {
            return compile_ident(c, n, member->module_access_definition, ref);
        }

        if (member->method && member->lhs->type.is_meta) {
            return compile_fn(c, member->method);
        }

        LLVMValueRef lhs = NULL;
        LLVMTypeRef  lhs_type = NULL;

        if (member->lhs->type.ref) {
            lhs = compile_expr(c, member->lhs, false);
            set_debug_pos(c, n->token.pos);

            LLVMTypeRef llvm_type_ptr = LLVMPointerTypeInContext(c->llvm_context, 0);
            for (size_t i = 1; i < member->lhs->type.ref; i++) {
                lhs = LLVMBuildLoad2(c->llvm_builder, llvm_type_ptr, lhs, "");
            }

            Type type = member->lhs->type;
            type.ref = 0;
            type.llvm = NULL;

            compile_type(c, &type);
            lhs_type = type.llvm;
        } else {
            lhs = compile_expr(c, member->lhs, !member->method);
            lhs_type = member->lhs->type.llvm;
            set_debug_pos(c, n->token.pos);
        }

        if (member->is_trait) {
            LLVMTypeRef ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);
            member->method_receiver_llvm = LLVMBuildLoad2(
                c->llvm_builder, ptr_type, LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, 1, ""), "");

            LLVMValueRef impl = LLVMBuildLoad2(
                c->llvm_builder, ptr_type, LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, 2, ""), "");

            // Impl check
            {
                LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

                LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntNE, impl, LLVMConstNull(ptr_type), "");
                LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                // Failure
                LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                {
                    const char *message = arena_sprintf(
                        &temp_arena, Pos_Fmt "Cannot access method of null trait\n", Pos_Arg(n->token.pos));

                    compile_panic(c, message, NULL, NULL, NULL);
                    arena_reset(&temp_arena, message);
                }

                // Success
                LLVMPositionBuilderAtEnd(c->llvm_builder, success);
            }

            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), member->trait_method, true)};

            return LLVMBuildLoad2(
                c->llvm_builder,
                ptr_type,
                LLVMBuildGEP2(c->llvm_builder, ptr_type, impl, indices, len(indices), ""),
                "");
        }

        if (member->method) {
            assert(member->method->node.type.kind == TYPE_FN);
            const Type_Fn *spec = member->method->node.type.spec.fn;

            assert(spec->args_count);
            const size_t expected_ref = spec->args[0].type.ref;
            const size_t actual_ref = member->lhs->type.ref;

            LLVMValueRef value = lhs;
            if (actual_ref < expected_ref) {
                value = undo_load(value);
            } else if (actual_ref > expected_ref) {
                for (size_t i = expected_ref; i + 1 < actual_ref; i++) {
                    value = LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), value, "");
                }
                value = LLVMBuildLoad2(c->llvm_builder, member->lhs->type.llvm, value, "");
            }

            member->method_receiver_llvm = value;
            return compile_fn(c, member->method);
        }

        if (member->rhs) {
            // Check if tag matches
            {
                LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

                if (member->lhs->type.kind == TYPE_TRAIT) {
                    LLVMTypeRef  ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);
                    LLVMValueRef tag = compile_load_if_not_null(c, lhs, ptr_type);

                    LLVMValueRef check =
                        LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, tag, compile_type_info(c, &n->type), "");
                    LLVMBuildCondBr(c->llvm_builder, check, success, failure);
                } else if (member->union_index) {
                    LLVMTypeRef  i64_type = LLVMInt64TypeInContext(c->llvm_context);
                    LLVMValueRef tag = LLVMBuildLoad2(c->llvm_builder, i64_type, lhs, "");

                    LLVMValueRef check = LLVMBuildICmp(
                        c->llvm_builder, LLVMIntEQ, tag, LLVMConstInt(i64_type, member->union_index, true), "");
                    LLVMBuildCondBr(c->llvm_builder, check, success, failure);
                } else {
                    unreachable();
                }

                // Failure
                LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                {
                    // TODO: Now that we have RTTI, this can be a better error message, like the one in constant
                    // expressions
                    const char *message = arena_sprintf(&temp_arena, Pos_Fmt "Type mismatch\n", Pos_Arg(n->token.pos));
                    compile_panic(c, message, NULL, NULL, NULL);
                    arena_reset(&temp_arena, message);
                }

                // Success
                LLVMPositionBuilderAtEnd(c->llvm_builder, success);
            }

            LLVMValueRef payload = LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, 1, "");
            if (member->lhs->type.kind == TYPE_TRAIT) {
                payload = LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), payload, "");
            }

            if (ref) {
                return payload;
            }
            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, payload, "");
        }

        if (member->lhs->type.kind == TYPE_ARRAY) {
            switch (member->field_index) {
            case 0:
                return lhs;

            case 1:
                return LLVMConstInt(n->type.llvm, member->lhs->type.spec.array.count, type_is_signed(n->type));

            default:
                unreachable();
            }
        }

        LLVMValueRef ptr = LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, member->field_index, "");
        if (ref) {
            return ptr;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, ptr, "");
    }

    case NODE_IMPORT:
        unreachable();

    case NODE_DISTINCT:
        unreachable();

    case NODE_INTERPOLATION: {
        Node_Interpolation *interp = (Node_Interpolation *) n;

        assert(c->interpolated_string_type.kind == TYPE_SLICE);
        LLVMTypeRef  element_type = compile_type(c, c->interpolated_string_type.spec.slice.element);
        LLVMValueRef memory = compile_alloca(c, LLVMArrayType(element_type, interp->children_count));

        size_t iota = 0;
        ll_foreach(it, &interp->children) {
            LLVMValueRef value = compile_expr(c, it, false);
            LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), iota++, true)};
            LLVMValueRef ptr = LLVMBuildGEP2(c->llvm_builder, element_type, memory, indices, len(indices), "");
            LLVMBuildStore(c->llvm_builder, value, ptr);
        }

        LLVMValueRef slice = compile_alloca(c, n->type.llvm);
        LLVMBuildStore(c->llvm_builder, memory, slice);
        LLVMBuildStore(
            c->llvm_builder,
            LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), interp->children_count, true),
            LLVMBuildStructGEP2(c->llvm_builder, n->type.llvm, slice, 1, ""));

        if (ref) {
            return slice;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, slice, "");
    }

    case NODE_FN:
        return compile_fn(c, (Node_Fn *) n);

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;

        LLVMValueRef memory = compile_alloca(c, n->type.llvm);
        LLVMBuildStore(c->llvm_builder, LLVMConstNull(n->type.llvm), memory);

        size_t ordered_iota = 0;
        for (Node *iter = compound->children.head; iter; iter = iter->next) {
            size_t it_iota = 0;
            if (!compound->is_designated) {
                it_iota = ordered_iota++;
            }

            Node *it = iter;
            if (compound->is_designated) {
                assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
                Node_Binary *it_binary = (Node_Binary *) it;
                it_iota = it->token.as.integer;
                it = it_binary->rhs;
            }

            LLVMValueRef ptr = NULL;
            if (n->type.kind == TYPE_STRUCT) {
                ptr = LLVMBuildStructGEP2(c->llvm_builder, n->type.llvm, memory, it_iota, "");
            } else if (n->type.kind == TYPE_ARRAY) {
                LLVMTypeRef  element_type = n->type.spec.array.element->llvm;
                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), it_iota, true),
                };
                ptr = LLVMBuildGEP2(c->llvm_builder, element_type, memory, indices, len(indices), "");
            } else {
                unreachable();
            }

            LLVMValueRef value = compile_expr(c, it, false);
            LLVMBuildStore(c->llvm_builder, value, ptr);
        }

        if (ref) {
            return memory;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
    }

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        if (call->is_type_cast) {
            Node *from = call->args.head;
            if (call->type_cast == TYPE_CAST_NOP) {
                return compile_expr(c, from, ref);
            }

            LLVMValueRef from_value = compile_expr(c, from, false);
            LLVMTypeRef  from_type = from->type.llvm;

            set_debug_pos(c, call->fn->token.pos);
            static_assert(COUNT_TYPE_CASTS == 5, "");
            switch (call->type_cast) {
            case TYPE_CAST_NORMAL:
                set_debug_pos(c, n->token.pos);
                return compile_cast(c, from_value, n->type.llvm, type_is_signed(from->type));

            case TYPE_CAST_TO_BOOL:
                set_debug_pos(c, n->token.pos);
                return LLVMBuildICmp(c->llvm_builder, LLVMIntNE, from_value, LLVMConstNull(from_type), "");

            case TYPE_CAST_TO_TRAIT:
                return compile_cast_to_trait(c, &call->args.head->type, call->type_cast_trait_impl, from_value, ref);

            case TYPE_CAST_TO_UNION:
                return compile_cast_to_union(c, n->type.llvm, call->type_cast_union_index, from_value, ref);

            default:
                unreachable();
            }
        }

        const void *checkpoint = arena_alloc(&temp_arena, 0);

        Typed_LLVM_Value fn = {0};
        fn.value = compile_expr(c, call->fn, false);
        fn.type = &call->fn->type;
        const Type_Fn *fn_spec = fn.type->spec.fn;

        size_t args_count = fn_spec->args_count;
        if (fn_spec->variadics_kind == VARIADICS_UNTYPED) {
            args_count = max(args_count, call->args_count);
        }
        Typed_LLVM_Value *args = arena_alloc(&temp_arena, args_count * sizeof(*args));

        bool   is_trait_call = false;
        size_t args_iota = 0;
        if (call->fn->kind == NODE_MEMBER) {
            Node_Member *member = (Node_Member *) call->fn;
            if ((member->method || member->is_trait) && !member->lhs->type.is_meta) {
                assert(member->method_receiver_llvm);
                args[args_iota].value = member->method_receiver_llvm;

                assert(fn_spec->args_count);
                args[args_iota].type = &fn_spec->args[0].type;
                args_iota++;
            }
            is_trait_call = member->is_trait;
        }

        LLVMTypeRef  variadics_type = NULL;
        LLVMValueRef variadics_memory = NULL;
        if (fn_spec->variadics_kind == VARIADICS_TYPED && !call->do_not_allocate_typed_variadic_array) {
            Type *type = &fn_spec->args[fn_spec->variadics_index].type;
            assert(type->kind == TYPE_SLICE);
            variadics_type = compile_type(c, type->spec.slice.element);

            if (call->typed_variadics_array_count) {
                variadics_memory = compile_alloca(c, LLVMArrayType(variadics_type, call->typed_variadics_array_count));
            } else {
                variadics_memory = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
            }

            LLVMValueRef variadics_slice = compile_alloca(c, c->llvm_slice_type);
            LLVMBuildStore(c->llvm_builder, variadics_memory, variadics_slice);
            LLVMBuildStore(
                c->llvm_builder,
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), call->typed_variadics_array_count, true),
                LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, variadics_slice, 1, ""));

            Typed_LLVM_Value arg = {0};
            arg.type = type;
            arg.value = LLVMBuildLoad2(c->llvm_builder, c->llvm_slice_type, variadics_slice, "");
            args[fn_spec->variadics_index] = arg;
        }

        for (Node *arg = call->args.head; arg; arg = arg->next) {
            if (arg->kind == NODE_BINARY && arg->token.kind == TOKEN_SET) {
                const size_t index = arg->token.as.integer;

                LLVMValueRef expr = compile_expr(c, ((Node_Binary *) arg)->rhs, false);
                args[index].type = &fn_spec->args[index].type;
                args[index].value = expr;

                // No point in advancing iota further, since the parser guarantees there will be no more positional
                // arguments
                continue;
            }

            const size_t group_values_count_save = c->group_values.count;

            LLVMValueRef expr = compile_expr(c, arg, false);
            if (arg->type.kind == TYPE_GROUP) {
                Type_Group *group = &arg->type.spec.group;
                assert(c->group_values.count == group_values_count_save + group->count);
                for (size_t i = 0; i < group->count; i++) {
                    Typed_LLVM_Value tv = {0};
                    tv.type = &group->data[i];
                    tv.value = c->group_values.data[group_values_count_save + i];
                    if (variadics_memory && args_iota >= fn_spec->variadics_index) {
                        LLVMValueRef indices[] = {
                            LLVMConstInt(
                                LLVMInt64TypeInContext(c->llvm_context), args_iota - fn_spec->variadics_index, true),
                        };

                        LLVMValueRef dst =
                            LLVMBuildGEP2(c->llvm_builder, variadics_type, variadics_memory, indices, len(indices), "");
                        LLVMBuildStore(c->llvm_builder, tv.value, dst);
                    } else {
                        args[args_iota] = tv;
                    }
                    args_iota++;
                }
            } else {
                Typed_LLVM_Value tv = {0};
                tv.type = &arg->type;
                tv.value = expr;
                if (variadics_memory && args_iota >= fn_spec->variadics_index) {
                    LLVMValueRef indices[] = {
                        LLVMConstInt(
                            LLVMInt64TypeInContext(c->llvm_context), args_iota - fn_spec->variadics_index, true),
                    };

                    LLVMValueRef dst =
                        LLVMBuildGEP2(c->llvm_builder, variadics_type, variadics_memory, indices, len(indices), "");
                    LLVMBuildStore(c->llvm_builder, tv.value, dst);
                } else {
                    args[args_iota] = tv;
                }
                args_iota++;
            }

            c->group_values.count = group_values_count_save;
        }

        compile_optional_arguments(c, args, fn_spec, call->fn->token.pos);

        const bool   is_group = n->type.kind == TYPE_GROUP;
        LLVMValueRef result = compile_call(c, fn, args, args_count, is_trait_call, ref || is_group);
        if (is_group) {
            assert(!ref);
            compile_type(c, &n->type);
            Type_Group *spec = &n->type.spec.group;
            for (size_t i = 0; i < spec->count; i++) {
                LLVMValueRef ptr = LLVMBuildStructGEP2(c->llvm_builder, spec->llvm, result, i, "");
                da_push(&c->group_values, LLVMBuildLoad2(c->llvm_builder, spec->data[i].llvm, ptr, ""));
            }
            result = NULL;
        }

        arena_reset(&temp_arena, checkpoint);
        return result;
    }

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        if (index->overload) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef lhs = compile_expr(c, index->lhs, false);
            LLVMValueRef a = compile_expr(c, index->a, false);
            LLVMValueRef b = compile_expr(c, index->b, false);

            Typed_LLVM_Value fn = {0};
            fn.value = compile_fn(c, index->overload);
            fn.type = &index->overload->node.type;

            const Type_Fn    *fn_spec = fn.type->spec.fn;
            Typed_LLVM_Value *args = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*args));
            if (fn_spec->args[0].type.ref > index->lhs->type.ref) {
                lhs = undo_load(lhs);
            }

            args[0].value = lhs;
            args[0].type = &fn_spec->args[0].type;

            if (a) {
                args[1].value = a;
                args[1].type = &fn_spec->args[1].type;
            }

            if (index->is_ranged) {
                if (b) {
                    args[2].value = b;
                    args[2].type = &fn_spec->args[2].type;
                }
            } else {
                args[2].value = LLVMConstInt(LLVMInt1TypeInContext(c->llvm_context), index->is_assign, true);
                args[2].type = &fn_spec->args[2].type;
            }

            compile_optional_arguments(c, args, fn_spec, n->token.pos);
            if (index->is_ranged) {
                LLVMValueRef value = compile_call(c, fn, args, fn_spec->args_count, false, ref);
                arena_reset(&temp_arena, checkpoint);
                return value;
            } else {
                LLVMValueRef ptr = compile_call(c, fn, args, fn_spec->args_count, false, false);
                arena_reset(&temp_arena, checkpoint);
                if (ref) {
                    return ptr;
                }
                return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, ptr, "");
            }
        }

        Type  element_type_buffer = {0};
        Type *element_type = &element_type_buffer;

        const char *label = "";
        if (index->lhs->type.ref) {
            element_type = n->type.spec.slice.element;
        } else {
            static_assert(COUNT_TYPES == 25, "");
            switch (index->lhs->type.kind) {
            case TYPE_ARRAY:
                label = "array";
                element_type = index->lhs->type.spec.array.element;
                break;

            case TYPE_SLICE:
                label = "slice";
                element_type = index->lhs->type.spec.slice.element;
                break;

            case TYPE_STRING:
                label = "string";
                element_type_buffer.kind = TYPE_CHAR;
                break;

            default:
                unreachable();
                break;
            }
        }

        LLVMValueRef lhs = compile_expr(c, index->lhs, index->lhs->type.kind == TYPE_ARRAY || !index->lhs->type.ref);
        LLVMValueRef a = compile_expr(c, index->a, false);

        compile_type(c, element_type);
        if (index->is_ranged) {
            if (a) {
                a = compile_cast(c, a, LLVMInt64TypeInContext(c->llvm_context), type_is_signed(index->a->type));
            } else {
                a = LLVMConstNull(LLVMInt64TypeInContext(c->llvm_context));
            }

            LLVMValueRef b = compile_expr(c, index->b, false);
            if (b) {
                b = compile_cast(c, b, LLVMInt64TypeInContext(c->llvm_context), type_is_signed(index->b->type));
            }

            set_debug_pos(c, n->token.pos);

            LLVMValueRef ptr = NULL;
            LLVMValueRef count = NULL;
            if (index->lhs->type.ref) {
                ptr = lhs;
            } else if (index->lhs->type.kind == TYPE_ARRAY) {
                ptr = lhs;
                count = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), index->lhs->type.spec.array.count, true);

                if (!b) {
                    b = count;
                }
            } else if (index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) {
                ptr = LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), lhs, "");
                count = LLVMBuildLoad2(
                    c->llvm_builder,
                    LLVMInt64TypeInContext(c->llvm_context),
                    LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, lhs, 1, ""),
                    "");

                if (!b) {
                    b = count;
                }
            } else {
                unreachable();
            }

            // Check if bounds are ascending
            {
                LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

                LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, a, b, "");
                LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                // Failure
                LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                {
                    const char *message = arena_sprintf(
                        &temp_arena,
                        Pos_Fmt "Range (%%zd..%%zd) is invalid: Beginning of range is more than end\n",
                        Pos_Arg(n->token.pos));

                    compile_panic(c, message, a, b, NULL);
                    arena_reset(&temp_arena, message);
                }

                // Success
                LLVMPositionBuilderAtEnd(c->llvm_builder, success);
            }

            if (count) {
                // Bounds check
                {
                    LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                    LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

                    LLVMValueRef check_begin_of_a =
                        LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, a, LLVMConstNull(LLVMTypeOf(a)), "");
                    LLVMValueRef check_end_of_a = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, a, count, "");
                    LLVMValueRef check_a = LLVMBuildAnd(c->llvm_builder, check_begin_of_a, check_end_of_a, "");

                    LLVMValueRef check_begin_of_b =
                        LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, b, LLVMConstNull(LLVMTypeOf(b)), "");
                    LLVMValueRef check_end_of_b = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, b, count, "");
                    LLVMValueRef check_b = LLVMBuildAnd(c->llvm_builder, check_begin_of_b, check_end_of_b, "");

                    LLVMValueRef check = LLVMBuildAnd(c->llvm_builder, check_a, check_b, "");
                    LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                    // Failure
                    LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                    {
                        const char *message = arena_sprintf(
                            &temp_arena,
                            Pos_Fmt "Range (%%zd..%%zd) is out of bounds in %s of length %%zd\n",
                            Pos_Arg(n->token.pos),
                            label);

                        compile_panic(c, message, a, b, count);
                        arena_reset(&temp_arena, message);
                    }

                    // Success
                    LLVMPositionBuilderAtEnd(c->llvm_builder, success);
                }
            }

            LLVMValueRef slice_data = LLVMBuildGEP2(c->llvm_builder, element_type->llvm, ptr, &a, 1, "");
            LLVMValueRef slice_count = LLVMBuildSub(c->llvm_builder, b, a, "");
            LLVMValueRef slice_struct = compile_alloca(c, n->type.llvm);
            LLVMBuildStore(c->llvm_builder, slice_data, slice_struct);
            LLVMBuildStore(
                c->llvm_builder,
                slice_count,
                LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, slice_struct, 1, ""));

            if (ref) {
                return slice_struct;
            }

            return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, slice_struct, "");
        }

        set_debug_pos(c, n->token.pos);

        // Bounds check
        {
            LLVMValueRef count = NULL;
            if (index->lhs->type.kind == TYPE_ARRAY) {
                count = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), index->lhs->type.spec.array.count, true);
            } else if (index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) {
                count = LLVMBuildStructGEP2(c->llvm_builder, index->lhs->type.llvm, lhs, 1, "");
                count = LLVMBuildLoad2(c->llvm_builder, LLVMInt64TypeInContext(c->llvm_context), count, "");
            } else {
                unreachable();
            }

            LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

            LLVMValueRef check_begin_of_a =
                LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, a, LLVMConstNull(LLVMTypeOf(a)), "");
            LLVMValueRef check_end_of_a = LLVMBuildICmp(c->llvm_builder, LLVMIntSLT, a, count, "");
            LLVMValueRef check = LLVMBuildAnd(c->llvm_builder, check_begin_of_a, check_end_of_a, "");

            LLVMBuildCondBr(c->llvm_builder, check, success, failure);

            // Failure
            LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
            {
                const char *message = arena_sprintf(
                    &temp_arena,
                    Pos_Fmt "Index %%zd is out of bounds in %s of length %%zd\n",
                    Pos_Arg(n->token.pos),
                    label);

                compile_panic(c, message, a, count, NULL);
                arena_reset(&temp_arena, message);
            }

            // Success
            LLVMPositionBuilderAtEnd(c->llvm_builder, success);
        }

        LLVMValueRef ptr = NULL;
        if (index->lhs->type.kind == TYPE_ARRAY) {
            ptr = lhs;
        } else if (index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) {
            ptr = LLVMBuildLoad2(c->llvm_builder, LLVMPointerTypeInContext(c->llvm_context, 0), lhs, "");
        } else {
            unreachable();
        }
        ptr = LLVMBuildGEP2(c->llvm_builder, element_type->llvm, ptr, &a, 1, "");

        if (ref) {
            return ptr;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, ptr, "");
    }

    case NODE_INDEXABLE:
        unreachable();

    default:
        unreachable();
        break;
    }
}

static LLVMValueRef compile_auto_cast(Compiler *c, Node *n, LLVMValueRef result, Auto_Cast *auto_cast, bool ref) {
    static_assert(COUNT_AUTO_CASTS == 4, "");
    switch (auto_cast->kind) {
    case AUTO_CAST_TO_TRAIT: {
        result = compile_cast_to_trait(c, &auto_cast->from, auto_cast->trait_impl, result, ref);
        n->type = auto_cast->to;
        compile_type(c, &n->type);
        return result;
    }

    case AUTO_CAST_TO_UNION: {
        n->type = auto_cast->to;
        compile_type(c, &n->type);
        return compile_cast_to_union(c, n->type.llvm, auto_cast->union_index, result, ref);
    }

    case AUTO_CAST_ARRAY_TO_SLICE: {
        LLVMValueRef memory = undo_load(result);
        assert(auto_cast->from.kind == TYPE_ARRAY);

        LLVMValueRef slice = compile_alloca(c, c->llvm_slice_type);
        LLVMBuildStore(c->llvm_builder, memory, slice);
        LLVMBuildStore(
            c->llvm_builder,
            LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), auto_cast->from.spec.array.count, true),
            LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, slice, 1, ""));

        n->type = auto_cast->to;
        compile_type(c, &n->type);

        if (ref) {
            return slice;
        }
        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, slice, "");
    }

    default:
        unreachable();
    }
}

static LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    if (!n->auto_casts) {
        return compile_expr_impl(c, n, ref);
    }

    if (n->auto_casts_count == 1) {
        n->type = n->auto_casts[0].from;
    } else {
        assert(n->type.kind == TYPE_GROUP);
        Type_Group *spec = &n->type.spec.group;
        for (size_t i = 0; i < spec->count; i++) {
            Auto_Cast *it = &n->auto_casts[i];
            if (it->kind != AUTO_CAST_NONE) {
                spec->data[i] = n->auto_casts[i].from;
            }
        }
    }

    const size_t group_values_start = c->group_values.count;
    const Type   n_type_save = n->type;

    LLVMValueRef result = compile_expr_impl(c, n, false);
    if (n->type.kind == TYPE_GROUP) {
        for (size_t i = group_values_start; i < c->group_values.count; i++) {
            Auto_Cast *it = &n->auto_casts[i - group_values_start];
            if (it->kind != AUTO_CAST_NONE) {
                c->group_values.data[i] = compile_auto_cast(c, n, c->group_values.data[i], it, ref);
                n->type = n_type_save;
            }
        }

        Type_Group *spec = &n->type.spec.group;
        for (size_t i = 0; i < spec->count; i++) {
            Auto_Cast *it = &n->auto_casts[i];
            if (it->kind != AUTO_CAST_NONE) {
                spec->data[i] = n->auto_casts[i].to;
            }
        }
    } else {
        result = compile_auto_cast(c, n, result, &n->auto_casts[0], ref);
    }
    return result;
}

static void introduce_ghost_for_union(Compiler *c, Node_Atom *ghost, bool is_trait) {
    LLVMValueRef real = compile_ident(c, (Node *) ghost, ghost->definition, true);
    if (is_trait) {
        ghost->ghost_llvm = LLVMBuildLoad2(
            c->llvm_builder,
            LLVMPointerTypeInContext(c->llvm_context, 0),
            LLVMBuildStructGEP2(c->llvm_builder, c->llvm_trait_type, real, 1, ""),
            "");
    } else {
        LLVMTypeRef  i64_type = LLVMInt64TypeInContext(c->llvm_context);
        LLVMValueRef indices[] = {LLVMConstInt(i64_type, 1, true)};
        ghost->ghost_llvm = LLVMBuildGEP2(c->llvm_builder, i64_type, real, indices, len(indices), "");
    }
}

static LLVMValueRef get_type_id_from_type_info_pointer(Compiler *c, LLVMValueRef ptr) {
    LLVMValueRef is_null =
        LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, ptr, LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0)), "");

    LLVMBasicBlockRef null_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBasicBlockRef not_null_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
    LLVMBuildCondBr(c->llvm_builder, is_null, null_block, not_null_block);

    LLVMPositionBuilderAtEnd(c->llvm_builder, null_block);
    LLVMBuildBr(c->llvm_builder, merge_block);

    LLVMPositionBuilderAtEnd(c->llvm_builder, not_null_block);
    LLVMValueRef id = LLVMBuildStructGEP2(c->llvm_builder, c->type_info_type.llvm, ptr, 3, "");
    id = LLVMBuildLoad2(c->llvm_builder, LLVMInt64TypeInContext(c->llvm_context), id, "");
    LLVMBuildBr(c->llvm_builder, merge_block);

    LLVMPositionBuilderAtEnd(c->llvm_builder, merge_block);
    LLVMTypeRef       i64_type = LLVMInt64TypeInContext(c->llvm_context);
    LLVMValueRef      phi = LLVMBuildPhi(c->llvm_builder, i64_type, "");
    LLVMValueRef      phi_values[] = {LLVMConstNull(i64_type), id};
    LLVMBasicBlockRef phi_blocks[] = {null_block, not_null_block};
    LLVMAddIncoming(phi, phi_values, phi_blocks, len(phi_blocks));
    return phi;
}

static_assert(COUNT_NODES == 28, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT:
        // Pass
        break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        if (define->is_const) {
            return;
        }

        if (define->is_value_known_at_compile_time) {
            Node_Atom *lhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                if (!lhs->definition_spec->llvm) {
                    compile_var_def(c, lhs);
                }
            }
        } else {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef *vars = NULL;
            if (define->expr) {
                vars = arena_alloc(&temp_arena, define->count * sizeof(*vars));
            }

            Node_Atom *lhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                assert(!lhs->definition_spec->llvm); // These are local variables, so compiled in an ordered fashion
                compile_var_def(c, lhs);
                if (define->expr) {
                    vars[lhs->definition_spec->group_index] = lhs->definition_spec->llvm;
                }
            }

            if (define->expr) {
                const size_t group_values_count_save = c->group_values.count;

                LLVMValueRef value = compile_expr(c, define->expr, false);
                set_debug_pos(c, define->node.token.pos);
                if (define->count == 1) {
                    LLVMBuildStore(c->llvm_builder, value, vars[0]);
                } else {
                    for (size_t i = 0; i < define->count; i++) {
                        LLVMValueRef value = c->group_values.data[group_values_count_save + i];
                        LLVMBuildStore(c->llvm_builder, value, vars[i]);
                    }
                }

                c->group_values.count = group_values_count_save;
            }

            arena_reset(&temp_arena, checkpoint);
        }
    } break;

    case NODE_BLOCK: {
        const size_t defers_count_save = c->defers.count;

        LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;
        c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
            c->llvm_debug_builder,
            c->llvm_debug_scope,
            get_debug_file(c, n->token.pos.path),
            n->token.pos.row + 1,
            n->token.pos.col + 1);

        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            compile_stmt(c, it);
        }

        compile_defers(c, defers_count_save, true);
        c->llvm_debug_scope = llvm_debug_scope_save;
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            if (iff->compile_time_real) {
                if (iff->compile_time_real->kind == NODE_IF) {
                    compile_stmt(c, iff->compile_time_real);
                } else if (iff->compile_time_real->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real;
                    for (Node *it = block->body.head; it; it = it->next) {
                        compile_stmt(c, it);
                    }
                } else {
                    unreachable();
                }
            }
            return;
        }

        LLVMBasicBlockRef consequence = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef antecedence = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        LLVMBasicBlockRef end = antecedence;
        if (iff->antecedence) {
            end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        }

        // Condition
        LLVMValueRef condition = compile_expr(c, iff->condition, false);
        set_debug_pos(c, n->token.pos);
        LLVMBuildCondBr(c->llvm_builder, condition, consequence, antecedence);

        // Consequence
        LLVMPositionBuilderAtEnd(c->llvm_builder, consequence);
        if (iff->context_replace.to) {
            introduce_ghost_for_union(
                c, iff->context_replace.to, iff->context_replace.from->node.type.kind == TYPE_TRAIT);
        }
        compile_stmt(c, iff->consequence);
        LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
        LLVMBuildBr(c->llvm_builder, end);

        // Antecedence
        if (iff->antecedence) {
            LLVMPositionBuilderAtEnd(c->llvm_builder, antecedence);
            compile_stmt(c, iff->antecedence);

            LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
            LLVMBuildBr(c->llvm_builder, end);
        }

        // End
        LLVMPositionBuilderAtEnd(c->llvm_builder, end);
    } break;

    case NODE_FOR: {
        LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;

        Node_For *forr = (Node_For *) n;
        if (forr->init) {
            c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
                c->llvm_debug_builder,
                c->llvm_debug_scope,
                get_debug_file(c, n->token.pos.path),
                n->token.pos.row + 1,
                n->token.pos.col + 1);

            compile_stmt(c, forr->init);
        }

        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        LLVMBasicBlockRef start = body;
        LLVMBasicBlockRef update = start;
        if (forr->update) {
            update = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        }

        LLVMBasicBlockRef llvm_loop_break_save = c->llvm_loop_break;
        c->llvm_loop_break = end;

        LLVMBasicBlockRef llvm_loop_condition_save = c->llvm_loop_continue;
        c->llvm_loop_continue = update;

        size_t loop_defers_start_save = c->loop_defers_start;
        {
            // Condition
            if (forr->condition) {
                start = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
                LLVMBuildBr(c->llvm_builder, start);
                LLVMPositionBuilderAtEnd(c->llvm_builder, start);
                LLVMBuildCondBr(c->llvm_builder, compile_expr(c, forr->condition, false), body, end);
            } else {
                LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
                LLVMBuildBr(c->llvm_builder, body);
            }

            // Body
            LLVMPositionBuilderAtEnd(c->llvm_builder, body);
            compile_stmt(c, forr->body);

            // Update
            if (forr->update) {
                LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
                LLVMBuildBr(c->llvm_builder, update);

                LLVMPositionBuilderAtEnd(c->llvm_builder, update);
                compile_expr(c, forr->update, false);
            }

            // Loop
            LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
            LLVMBuildBr(c->llvm_builder, start);

            // End
            LLVMPositionBuilderAtEnd(c->llvm_builder, end);
        }
        c->llvm_loop_break = llvm_loop_break_save;
        c->llvm_loop_continue = llvm_loop_condition_save;
        c->loop_defers_start = loop_defers_start_save;

        c->llvm_debug_scope = llvm_debug_scope_save;
    } break;

    case NODE_CASE:
        unreachable();

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            if (sw->compile_time_real) {
                assert(sw->compile_time_real->body->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) sw->compile_time_real->body;
                for (Node *it = block->body.head; it; it = it->next) {
                    compile_stmt(c, it);
                }
            }
            return;
        }

        LLVMTypeRef  i64_type = LLVMInt64TypeInContext(c->llvm_context);
        LLVMTypeRef  ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);
        LLVMValueRef expr = compile_expr(c, sw->expr, false);
        if (sw->trait) {
            expr = undo_load(expr);
            expr = LLVMBuildLoad2(c->llvm_builder, ptr_type, expr, "");
            expr = get_type_id_from_type_info_pointer(c, expr);
        } else if (sw->unionn) {
            expr = undo_load(expr);
            expr = LLVMBuildLoad2(c->llvm_builder, i64_type, expr, "");
        } else if (sw->is_expr_type_info) {
            expr = get_type_id_from_type_info_pointer(c, expr);
        }

        LLVMBasicBlockRef fallback = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        set_debug_pos(c, n->token.pos);
        LLVMValueRef sw_llvm = LLVMBuildSwitch(c->llvm_builder, expr, fallback, sw->preds_count);

        size_t iota = 0;
        for (Node *it = sw->cases.head; it; it = it->next) {
            Node_Case *branch = (Node_Case *) it;
            if (!branch->preds.head) {
                continue; // Fallback
            }

            LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            for (Node *pred = branch->preds.head; pred; pred = pred->next) {
                Const_Value *pred_value = &sw->preds[iota++].value;

                LLVMValueRef value = NULL;
                if (sw->unionn) {
                    value = LLVMConstInt(i64_type, i64_from_int128(pred_value->as.integer), true);
                } else if (sw->trait || sw->is_expr_type_info) {
                    if (pred_value->kind == CONST_VALUE_TYPE) {
                        assert(!pred_value->as.type.is_meta);
                        compile_type_info(c, &pred_value->as.type);

                        const size_t id = ht_get(&c->type_info_cache, pred_value->as.type)->id;
                        value = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), id, true);
                    } else if (pred_value->kind == CONST_VALUE_INT && int128_is_zero(pred_value->as.integer)) {
                        value = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), 0, true);
                    } else {
                        unreachable();
                    }
                } else {
                    value = compile_const_value(c, *pred_value, sw->expr->type);
                }
                LLVMAddCase(sw_llvm, value, block);
            }
            LLVMPositionBuilderAtEnd(c->llvm_builder, block);

            if (branch->context_replace.to) {
                introduce_ghost_for_union(
                    c, branch->context_replace.to, branch->context_replace.from->node.type.kind == TYPE_TRAIT);
            }
            compile_stmt(c, branch->body);
            LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
            LLVMBuildBr(c->llvm_builder, end);
        }
        assert(iota == sw->preds_count);

        LLVMPositionBuilderAtEnd(c->llvm_builder, fallback);
        if (sw->fallback) {
            compile_stmt(c, ((Node_Case *) sw->fallback)->body);
        }

        LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
        LLVMBuildBr(c->llvm_builder, end);

        LLVMPositionBuilderAtEnd(c->llvm_builder, end);
    } break;

    case NODE_JUMP:
        compile_defers(c, c->loop_defers_start, false);
        set_debug_pos(c, n->token.pos);
        if (n->token.kind == TOKEN_BREAK) {
            LLVMBuildBr(c->llvm_builder, c->llvm_loop_break);
            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));
        } else if (n->token.kind == TOKEN_CONTINUE) {
            LLVMBuildBr(c->llvm_builder, c->llvm_loop_continue);
            LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));
        } else {
            unreachable();
        }
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        da_push(&c->defers, defer->stmt);
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;

        const size_t group_values_count_save = c->group_values.count;
        LLVMValueRef value = compile_expr(c, returnn->value, false);
        if (type_is_compound(n->type)) {
            ABI_Info abi = get_abi_info_for_type(c, &n->type, false);
            if (n->type.kind == TYPE_GROUP) {
                const size_t count = n->type.spec.group.count;
                assert(c->group_values.count == group_values_count_save + count);

                LLVMValueRef memory = compile_alloca(c, n->type.llvm);
                for (size_t i = 0; i < count; i++) {
                    LLVMValueRef value = c->group_values.data[group_values_count_save + i];
                    LLVMValueRef ptr = LLVMBuildStructGEP2(c->llvm_builder, n->type.llvm, memory, i, "");
                    LLVMBuildStore(c->llvm_builder, value, ptr);
                }
                value = LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
            }

            static_assert(ABI_DIRECT_TYPES_MAX == 2, "");
            switch (abi.direct_types_count) {
            case 0:
                set_debug_pos(c, n->token.pos);
                LLVMBuildStore(c->llvm_builder, value, LLVMGetParam(c->llvm_fn, 0));
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRetVoid(c->llvm_builder);
                break;

            case 1: {
                LLVMTypeRef  abi_type = abi.direct_types[0];
                const size_t abi_size = LLVMABISizeOfType(c->llvm_target_data, abi_type);

                LLVMTypeRef  value_type = LLVMTypeOf(value);
                const size_t value_size = LLVMABISizeOfType(c->llvm_target_data, value_type);

                value = undo_load(value);
                if (abi_size > value_size) {
                    if (abi_size > 8) {
                        LLVMValueRef memory = compile_alloca(c, abi_type);
                        LLVMBuildMemCpy(
                            c->llvm_builder,
                            memory,
                            LLVMABIAlignmentOfType(c->llvm_target_data, abi_type),
                            value,
                            LLVMABIAlignmentOfType(c->llvm_target_data, value_type),
                            LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), value_size, true));
                        value = LLVMBuildLoad2(c->llvm_builder, abi_type, memory, "");
                    } else {
                        value = LLVMBuildLoad2(
                            c->llvm_builder, LLVMIntTypeInContext(c->llvm_context, value_size * 8), value, "");
                        value = LLVMBuildZExt(c->llvm_builder, value, abi_type, "");
                    }
                } else {
                    value = LLVMBuildLoad2(c->llvm_builder, abi_type, value, "");
                }
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
            } break;

            case 2: {
                LLVMTypeRef type =
                    LLVMStructTypeInContext(c->llvm_context, abi.direct_types, abi.direct_types_count, false);
                value = undo_load(value);
                value = LLVMBuildLoad2(c->llvm_builder, type, value, "");
                compile_defers(c, c->defers_start, false);
                set_debug_pos(c, n->token.pos);
                LLVMBuildRet(c->llvm_builder, value);
            } break;

            default:
                unreachable();
            }
        } else {
            set_debug_pos(c, n->token.pos);

            compile_defers(c, c->defers_start, false);
            set_debug_pos(c, n->token.pos);
            if (n->type.kind == TYPE_UNIT) {
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                LLVMBuildRet(c->llvm_builder, value);
            }
        }
        LLVMPositionBuilderAtEnd(c->llvm_builder, LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, ""));

        c->group_values.count = group_values_count_save;
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            compile_stmt(c, it);
        }
    } break;

    default: {
        const size_t group_values_count_save = c->group_values.count;
        compile_expr(c, n, false);
        c->group_values.count = group_values_count_save;
    } break;
    }
}

static void compiler_init_llvm_target_data(Compiler *c) {
    if (LLVMInitializeNativeTarget() != 0) {
        fprintf(stderr, "ERROR: Failed to initialize native target\n");
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
        fprintf(stderr, "ERROR: %s\n", error);
        exit(1);
    }

    c->llvm_target_machine = LLVMCreateTargetMachine(
        target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    c->llvm_target_data = LLVMCreateTargetDataLayout(c->llvm_target_machine);

    // Initialize the common types
    {
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

size_t compile_sizeof(Compiler *c, Type *type) {
    if (!c->llvm_target_data) {
        compiler_init_llvm_target_data(c);
    }

    compile_type(c, type);
    if (!LLVMTypeIsSized(type->llvm)) {
        return 0;
    }
    return LLVMABISizeOfType(c->llvm_target_data, type->llvm);
}

void compiler_build(Compiler *c, const char *output_path) {
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    assert(c->cmd);
    assert(c->modules);
    assert(c->main_fn);

    if (!c->llvm_context) {
        compiler_init_llvm_target_data(c);
    }
    c->llvm_builder = LLVMCreateBuilderInContext(c->llvm_context);

    c->llvm_attribute_sret = LLVMGetEnumAttributeKindForName("sret", strlen("sret"));
    c->llvm_attribute_byval = LLVMGetEnumAttributeKindForName("byval", strlen("byval"));
    c->llvm_attribute_alwaysinline = LLVMGetEnumAttributeKindForName("alwaysinline", strlen("alwaysinline"));

    c->llvm_debug_builder = LLVMCreateDIBuilder(c->llvm_module);

    c->llvm_debug_compile_unit = LLVMDIBuilderCreateCompileUnit(
        c->llvm_debug_builder,
        LLVMDWARFSourceLanguageC,
        get_debug_file(c, c->main_fn->node.token.pos.path),
        "glos",
        4,
        false,
        "",
        0,
        0,
        "",
        0,
        LLVMDWARFEmissionFull,
        0,
        0,
        0,
        "",
        0,
        "",
        0);

    compile_type(c, &c->type_info_type);

    const Const_Value main = get_const_definition_value(c, c->builtin_module, sv_from_cstr("main"), NULL);
    assert(main.kind == CONST_VALUE_FN);
    compile_fn(c, main.as.fn);

    LLVMPassBuilderOptionsRef pass_builder_options = LLVMCreatePassBuilderOptions();
    LLVMRunPasses(c->llvm_module, "always-inline", c->llvm_target_machine, pass_builder_options);
    LLVMDisposePassBuilderOptions(pass_builder_options);

    LLVMDIBuilderFinalize(c->llvm_debug_builder);
    LLVMDisposeDIBuilder(c->llvm_debug_builder);

    const char *object_path = temp_replace_suffix(output_path, EXE_FILE_EXTENSION, OBJ_FILE_EXTENSION);
    temporary_files_push(object_path);
    {
        // TODO: Remove
        // LLVMPrintModuleToFile(c->llvm_module, "/dev/stdout", NULL);

        char *error = NULL;
        if (LLVMVerifyModule(c->llvm_module, LLVMReturnStatusAction, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        if (LLVMTargetMachineEmitToFile(c->llvm_target_machine, c->llvm_module, object_path, LLVMObjectFile, &error)) {
            fprintf(stderr, "ERROR: %s\n", error);
            exit(1);
        }

        LLVMDisposeTargetData(c->llvm_target_data);
        LLVMDisposeTargetMachine(c->llvm_target_machine);

        LLVMDisposeBuilder(c->llvm_builder);
        LLVMDisposeModule(c->llvm_module);
        LLVMContextDispose(c->llvm_context);

#ifdef PLATFORM_X86_64_WINDOWS
        if (is_lld_available_in_path()) {
            cmd_push(c->cmd, "lld-link");
        } else {
            cmd_push(c->cmd, "link", "/nologo");
        }

        cmd_push(c->cmd, arena_sprintf(&temp_arena, "/out:%s", output_path));
        cmd_push(c->cmd, "/defaultlib:libcmt");
#else
        cmd_push(c->cmd, "cc");
        if (is_lld_available_in_path()) {
            cmd_push(c->cmd, "-fuse-ld=lld");
        }
        cmd_push(c->cmd, "-o", output_path);
#endif // PLATFORM_X86_64_WINDOWS

        cmd_push(c->cmd, object_path);
        cmd_push_many(c->cmd, c->link_flags->data, c->link_flags->count);

        const char *proc_name = c->cmd->data[0];
        Proc        proc = cmd_run_async(c->cmd, (Cmd_Stdio) {0});
        if (proc.id == PROC_INVALID) {
            fprintf(stderr, "ERROR: Could not execute '%s'. Make sure a C SDK is setup properly\n", proc_name);
            exit(1);
        }

        const int proc_code = cmd_wait(proc);
        if (proc_code != 0) {
            fprintf(stderr, "ERROR: Process '%s' exited abnormally with code %d\n", proc_name, proc_code);
            exit(1);
        }
    }

    ht_free(&c->llvm_debug_files);
    ht_free(&c->type_info_cache);
    ht_free(&c->methods_table);
    da_free(&c->methods_list);
    da_free(&c->context.locals);
    da_free(&c->struct_fields);
    da_free(&c->arg_values);
    da_free(&c->group_values);
    da_free(&c->defers);
    arena_reset(&temp_arena, checkpoint);
}
