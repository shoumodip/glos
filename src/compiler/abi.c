#include "compiler.h"

#ifdef PLATFORM_X86_64_LINUX
static_assert(COUNT_TYPES == 27, "");
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

    case TYPE_POLYMORPH:
        unreachable();

    default:
        unreachable();
    }
}
#endif // PLATFORM_X86_64_LINUX

ABI_Info get_abi_info_for_type(Compiler *c, Type *type, bool is_arg) {
    ABI_Info info = {0};
    size_t   size = compile_sizeof(c, type);

    info.type = type->llvm;
    if (type->ref) {
        info.direct_types[info.direct_types_count++] = LLVMPointerTypeInContext(c->llvm_context, 0);
        return info;
    }

    static_assert(COUNT_TYPES == 27, "");
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
