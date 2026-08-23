#define DONT_DEFINE_EXIT_WRAPPER
#include "../checker/checker.h"
#include "compiler.h"

static void put_scalar_type_into_qword(Compiler *c, ABI_Info *info, LLVMTypeRef type, size_t offset) {
    const size_t index = offset / 8;
    assert(index < len(info->direct_types));

    LLVMTypeRef *dst = &info->direct_types[index];
    if (!*dst) {
        info->direct_types_count++;
    }

    switch (LLVMGetTypeKind(type)) {
    case LLVMIntegerTypeKind:
        if (*dst) {
            const size_t minimum =
                LLVMABISizeOfType(c->llvm_target_data, *dst) + LLVMABISizeOfType(c->llvm_target_data, type);

            size_t size = 1;
            while (size < minimum) {
                size *= 2;
            }

            *dst = LLVMIntTypeInContext(c->llvm_context, size * 8);
        } else {
            *dst = type;
        }
        break;

    case LLVMFloatTypeKind:
        if (*dst) {
            if (LLVMGetTypeKind(*dst) == LLVMFloatTypeKind) {
#ifdef PLATFORM_X86_64_LINUX
                *dst = LLVMVectorType(*dst, 2);
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
                *dst = LLVMArrayType(*dst, 2);
#endif // PLATFORM_ARM64_MACOS
            } else {
                *dst = LLVMInt64TypeInContext(c->llvm_context);
            }
        } else {
            *dst = type;
        }
        break;

    case LLVMDoubleTypeKind:
        assert(!*dst);
        *dst = type;
        break;

    case LLVMPointerTypeKind:
        assert(!*dst);
        *dst = type;
        break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_TYPES == 30, "");
static void split_type_into_qwords(Compiler *c, ABI_Info *info, const Type *type, size_t offset, size_t size) {
    if (type->ref) {
        put_scalar_type_into_qword(c, info, LLVMPointerTypeInContext(c->llvm_context, 0), offset);
        return;
    }

    switch (type->kind) {
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:

    case TYPE_INT:
    case TYPE_ENUM:
        put_scalar_type_into_qword(c, info, LLVMIntTypeInContext(c->llvm_context, size * 8), offset);
        break;

    case TYPE_F32:
        put_scalar_type_into_qword(c, info, LLVMFloatTypeInContext(c->llvm_context), offset);
        break;

    case TYPE_F64:
    case TYPE_FLOAT:
        put_scalar_type_into_qword(c, info, LLVMDoubleTypeInContext(c->llvm_context), offset);
        break;

    case TYPE_FN:
    case TYPE_RAWPTR:
        put_scalar_type_into_qword(c, info, LLVMPointerTypeInContext(c->llvm_context, 0), offset);
        break;

    case TYPE_UNION:
        if (size > 8) {
            put_scalar_type_into_qword(c, info, LLVMInt64TypeInContext(c->llvm_context), offset);
            put_scalar_type_into_qword(c, info, LLVMIntTypeInContext(c->llvm_context, (size - 8) * 8), offset + 8);
        } else {
            put_scalar_type_into_qword(c, info, LLVMIntTypeInContext(c->llvm_context, size * 8), offset);
        }
        break;

    case TYPE_STRUCT: {
        const Type_Struct *spec = type->spec.structt;
        for (size_t i = 0; i < spec->fields_count; i++) {
            const Type_Struct_Field *it = &spec->fields[i];
            split_type_into_qwords(c, info, &it->type, offset + it->offset, it->size);
        }
    } break;

    case TYPE_ARRAY: {
        const Type_Array *spec = &type->spec.array;
        if (spec->count > 4) {
            if (size > 8) {
                put_scalar_type_into_qword(c, info, LLVMInt64TypeInContext(c->llvm_context), offset);
                put_scalar_type_into_qword(c, info, LLVMIntTypeInContext(c->llvm_context, (size - 8) * 8), offset + 8);
            } else {
                put_scalar_type_into_qword(c, info, LLVMIntTypeInContext(c->llvm_context, size * 8), offset);
            }
        } else {
            const size_t element_size = LLVMABISizeOfType(c->llvm_target_data, spec->element->llvm);
            for (size_t i = 0; i < spec->count; i++) {
                split_type_into_qwords(c, info, spec->element, offset + i * element_size, element_size);
            }
        }
    } break;

    case TYPE_SLICE:
    case TYPE_STRING:
        put_scalar_type_into_qword(c, info, LLVMPointerTypeInContext(c->llvm_context, 0), offset + 0);
        put_scalar_type_into_qword(c, info, LLVMInt64TypeInContext(c->llvm_context), offset + 8);
        break;

    case TYPE_GROUP: {
        const Type_Group *spec = &type->spec.group;
        for (size_t i = 0; i < spec->count; i++) {
            const Type *it = &spec->data[i];
            split_type_into_qwords(
                c, info, it, offset + spec->offsets[i], LLVMABISizeOfType(c->llvm_target_data, it->llvm));
        }
    } break;

    default:
        unreachable();
        break;
    }
}

#ifdef PLATFORM_ARM64_MACOS
static inline bool llvm_type_is_2xfloat(LLVMTypeRef type) {
    if (LLVMGetTypeKind(type) != LLVMArrayTypeKind) {
        return false;
    }

    return LLVMGetTypeKind(LLVMGetElementType(type)) == LLVMFloatTypeKind && LLVMGetArrayLength(type) == 2;
}
#endif // PLATFORM_ARM64_MACOS

ABI_Info get_abi_info_for_type(Compiler *c, Type *type, bool is_arg) {
    size_t   size = compile_sizeof(c, type);
    ABI_Info info = {.type = type->llvm};

#ifdef PLATFORM_X86_64_WINDOWS
    if (size > 8) {
        return info;
    }

    if (type_is_compound(*type)) {
        if (size == 1 || size == 2 || size == 4 || size == 8) {
            info.direct_types[info.direct_types_count++] = LLVMIntTypeInContext(c->llvm_context, size * 8);
        }

        // Any other sized type <8 bytes will be passed indirectly
        return info;
    }
#else
    if (size > 16) {
        return info;
    }
#endif // PLATFORM_X86_64_WINDOWS

    if (type->ref) {
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;
    }

    // Special cases
    static_assert(COUNT_TYPES == 30, "");
    switch (type->kind) {
    case TYPE_UNIT:
        info.direct_types[info.direct_types_count++] = LLVMVoidTypeInContext(c->llvm_context);
        return info;

    case TYPE_BOOL:
        info.direct_types[info.direct_types_count++] = LLVMInt1TypeInContext(c->llvm_context);
        return info;

    default:
        // Pass
        break;
    }

    split_type_into_qwords(c, &info, type, 0, size);
    assert(info.direct_types_count);

#ifdef PLATFORM_ARM64_MACOS
    if (info.direct_types_count == 1) {
        if (LLVMGetTypeKind(info.direct_types[0]) == LLVMIntegerTypeKind && type_is_compound(*type) && is_arg) {
            // Bytes(0 < N <= 8) And (QWORD is an integer) And (Type is Compound) And (Used in Argument) => i64
            info.direct_types[0] = LLVMInt64TypeInContext(c->llvm_context);
            return info;
        }
    }

    if (info.direct_types_count == 2) {
        const LLVMTypeRef  t0 = info.direct_types[0];
        const LLVMTypeRef  t1 = info.direct_types[1];
        const LLVMTypeKind k0 = LLVMGetTypeKind(t0);
        const LLVMTypeKind k1 = LLVMGetTypeKind(t1);

        // (double, double) => [2 x double]
        if (k0 == LLVMDoubleTypeKind && k1 == LLVMDoubleTypeKind) {
            info.direct_types[0] = LLVMArrayType(LLVMDoubleTypeInContext(c->llvm_context), 2);
            info.direct_types_count = 1;
            return info;
        }

        // ([2 x float], float) => [3 x float]
        if (llvm_type_is_2xfloat(t0) && k1 == LLVMFloatTypeKind) {
            info.direct_types[0] = LLVMArrayType(LLVMFloatTypeInContext(c->llvm_context), 3);
            info.direct_types_count = 1;
            return info;
        }

        // ([2 x float], [2 x float]) => [4 x float]
        if (llvm_type_is_2xfloat(t0) && llvm_type_is_2xfloat(t1)) {
            info.direct_types[0] = LLVMArrayType(LLVMFloatTypeInContext(c->llvm_context), 4);
            info.direct_types_count = 1;
            return info;
        }

        // In any other case => [2 x i64]
        info.direct_types[0] = LLVMArrayType(LLVMInt64TypeInContext(c->llvm_context), 2);
        info.direct_types_count = 1;
    }
#endif // PLATFORM_ARM64_MACOS

    return info;
    unused(is_arg); // The argument 'is_arg' is only relevant for macOS
}

void abi_set_return_type(Compiler *c, ABI *abi, Type *type) {
    assert(abi->actual_args_count == 0);
    abi->return_abi = get_abi_info_for_type(c, type, false);
    abi->return_type = type;
    if (!abi->return_abi.direct_types_count) {
        abi->actual_args_count++;
    }
}

void abi_set_argument_type(Compiler *c, ABI *abi, size_t index, Type *type) {
    assert(index < abi->args_count);
    ABI_Info *it = &abi->args[index];
    *it = get_abi_info_for_type(c, type, true);
    if (it->direct_types_count) {
        abi->actual_args_count += it->direct_types_count;
    } else {
        abi->actual_args_count++;
    }
}

void abi_set_variadic_at(ABI *abi, size_t index) {
    assert(!abi->is_variadic);
    abi->is_variadic = true;
    abi->variadics_start = index;
}

LLVMTypeRef abi_finalize(Compiler *c, ABI *abi) {
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

void compile_call_begin(Compiler *c, Call_Compiler *call, Typed_LLVM_Value fn, size_t args_count) {
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

void compile_call_arg(Compiler *c, Call_Compiler *call, size_t arg_index, Typed_LLVM_Value *arg) {
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
                    const bool is_signed = type_is_signed(*arg->type);
                    expr = compile_cast(c, expr, LLVMInt32TypeInContext(c->llvm_context), is_signed, is_signed);
                } else if (type_eq_without_distinct(*arg->type, (Type) {.kind = TYPE_F32})) {
                    // Promote f32 into f64
                    expr = LLVMBuildFPExt(c->llvm_builder, expr, LLVMDoubleTypeInContext(c->llvm_context), "");
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

LLVMValueRef compile_call_finalize(Compiler *c, Call_Compiler *call, bool raw, bool ref) {
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

LLVMValueRef compile_call(
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
