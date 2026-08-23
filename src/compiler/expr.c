#include "../checker.h"
#include "../error.h"
#include "compiler.h"

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

void compile_trait_impl(Compiler *c, Type_Trait_Impl *impl) {
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

typedef struct {
    size_t defers_start;
    size_t loop_defers_start;

    LLVMValueRef llvm_fn;
    LLVMValueRef llvm_fn_last_alloca;

    LLVMMetadataRef   llvm_debug_scope;
    LLVMBasicBlockRef llvm_current_block;

    LLVMMetadataRef llvm_current_debug_location;
} Compile_Fn_Backup;

static void compile_fn_backup_save(Compiler *c, Compile_Fn_Backup *b) {
    b->defers_start = c->defers_start;
    c->defers_start = c->defers.count;
    b->loop_defers_start = c->loop_defers_start;
    c->loop_defers_start = c->defers.count;

    b->llvm_fn = c->llvm_fn;
    b->llvm_fn_last_alloca = c->llvm_fn_last_alloca;

    b->llvm_debug_scope = c->llvm_debug_scope;
    b->llvm_current_block = LLVMGetInsertBlock(c->llvm_builder);

    b->llvm_current_debug_location = LLVMGetCurrentDebugLocation2(c->llvm_builder);
}

static void compile_fn_backup_restore(Compiler *c, const Compile_Fn_Backup *b) {
    c->defers.count = c->defers_start;
    c->defers_start = b->defers_start;
    c->loop_defers_start = b->loop_defers_start;

    c->llvm_fn = b->llvm_fn;
    c->llvm_fn_last_alloca = b->llvm_fn_last_alloca;

    c->llvm_debug_scope = b->llvm_debug_scope;
    LLVMPositionBuilderAtEnd(c->llvm_builder, b->llvm_current_block);
    LLVMSetCurrentDebugLocation2(c->llvm_builder, b->llvm_current_debug_location);
}

LLVMValueRef compile_ident(Compiler *c, Node *n, Node_Atom *definition, bool ref) {
    assert(definition);

    // Constant value
    {
        const Const_Value *const_value = NULL;
        if (definition->polymorph) {
            assert(definition->polymorph->is_monomorphized);
            const_value = &definition->polymorph->monomorphization_value;
        } else if (definition->definition_spec->is_const) {
            const_value = &definition->definition_spec->const_value;
        }

        if (const_value) {
            static_assert(COUNT_CONST_VALUES == 12, "");
            switch (const_value->kind) {
            case CONST_VALUE_TRAIT:
            case CONST_VALUE_UNION:
            case CONST_VALUE_STRUCT:
            case CONST_VALUE_ARRAY:
            case CONST_VALUE_DYNAMIC_ARRAY:
            case CONST_VALUE_STRING:
                if (!definition->definition_spec->llvm) {
                    definition->definition_spec->llvm =
                        compile_const_value_into_memory(c, compile_const_value(c, *const_value, n->type));
                }

                if (ref) {
                    return definition->definition_spec->llvm;
                }

                set_debug_pos(c, n->token.pos);
                return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, definition->definition_spec->llvm, "");

            case CONST_VALUE_POLYMORPH:
                unreachable();

            default:
                if (definition->polymorph) {
                    if (!definition->polymorph->llvm) {
                        definition->polymorph->llvm = compile_const_value(c, *const_value, n->type);
                    }
                    return definition->polymorph->llvm;
                }

                assert(definition->definition_spec);
                if (!definition->definition_spec->llvm) {
                    definition->definition_spec->llvm = compile_const_value(c, *const_value, n->type);
                }
                return definition->definition_spec->llvm;
            }
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

LLVMValueRef compile_fn(Compiler *c, Node_Fn *fn) {
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

        SV fn_name = sv_from_cstr(temp_nested_fn_name(fn, fn->module));
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

                    if (!it->node.type.llvm) {
                        compile_type(c, &it->node.type);
                    }

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
                set_debug_pos(c, block->end.pos);
                LLVMBuildRetVoid(c->llvm_builder);
            } else {
                // The semantic analyzer has already determined that the function returns in all execution paths.
                // No need to compile defers here, as this is unreachable.
                set_debug_pos(c, block->end.pos);
                LLVMBuildUnreachable(c->llvm_builder);
            }
        }

        compile_fn_backup_restore(c, &backup);
    }

    arena_reset(&temp_arena, checkpoint);
    return fn->llvm;
}

void compile_optional_arguments(Compiler *c, Typed_LLVM_Value *args, const Type_Fn *fn_spec, Pos caller_location) {
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
            static_assert(COUNT_CONST_VALUES == 12, "");
            switch (arg->default_value->kind) {
            case CONST_VALUE_TRAIT:
            case CONST_VALUE_UNION:
            case CONST_VALUE_STRUCT:
            case CONST_VALUE_ARRAY:
            case CONST_VALUE_DYNAMIC_ARRAY:
            case CONST_VALUE_STRING:
                if (!arg->default_value_llvm) {
                    arg->default_value_llvm =
                        compile_const_value_into_memory(c, compile_const_value(c, *arg->default_value, arg->type));
                }

                value = LLVMBuildLoad2(c->llvm_builder, arg->type.llvm, arg->default_value_llvm, "");
                break;

            case CONST_VALUE_POLYMORPH:
                unreachable();

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

LLVMValueRef compile_expr_atom(Compiler *c, Node_Atom *atom, bool ref) {
    Node *n = (Node *) atom;
    static_assert(COUNT_TOKENS == 78, "");
    switch (n->token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
        return LLVMConstInt(n->type.llvm, n->token.as.integer, type_is_signed(n->type));

    case TOKEN_NULL:
        return LLVMConstNull(n->type.llvm);

    case TOKEN_IDENT:
        return compile_ident(c, n, (Node_Atom *) atom->definition, ref);

    case TOKEN_STRING: {
        if (type_eq(n->type, (Type) {.kind = TYPE_CHAR, .ref = 1})) {
            return compile_const_value_into_memory(
                c, LLVMConstStringInContext(c->llvm_context, n->token.as.string.data, n->token.as.string.count, false));
        }

        LLVMValueRef memory =
            compile_const_value_into_memory(c, compile_string_into_const_value(c, n->token.as.string));
        if (ref) {
            return memory;
        }

        return LLVMBuildLoad2(c->llvm_builder, n->type.llvm, memory, "");
    }

    case TOKEN_ISTRING: {
        LLVMValueRef memory =
            compile_const_value_into_memory(c, compile_string_into_const_value(c, n->token.as.string));
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

LLVMValueRef compile_expr_unary(Compiler *c, Node_Unary *unary, bool ref) {
    Node *n = (Node *) unary;

    LLVMValueRef value = NULL;
    static_assert(COUNT_TOKENS == 78, "");
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

            compile_optional_arguments(c, args, fn_spec, get_leftmost_point_of_node(n));
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

    compile_optional_arguments(c, args, fn_spec, get_leftmost_point_of_node((Node *) binary));
    LLVMValueRef result = compile_call(c, fn, args, fn_spec->args_count, false, false);

    arena_reset(&temp_arena, checkpoint);
    return result;
}

LLVMValueRef compile_expr_binary(Compiler *c, Node_Binary *binary) {
    Node *n = (Node *) binary;
    if (binary->trait_check) {
        LLVMTypeRef  ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);
        LLVMValueRef value = LLVMBuildLoad2(c->llvm_builder, ptr_type, compile_expr(c, binary->trait_check, true), "");

        LLVMValueRef expected = NULL;
        if (binary->trait_check_type) {
            expected = compile_type_info(c, binary->trait_check_type);
        } else {
            expected = LLVMConstNull(ptr_type);
        }

        return LLVMBuildICmp(c->llvm_builder, n->token.kind == TOKEN_EQ ? LLVMIntEQ : LLVMIntNE, value, expected, "");
    }

    if (binary->union_check) {
        LLVMTypeRef  i64_type = LLVMInt64TypeInContext(c->llvm_context);
        LLVMValueRef value = LLVMBuildLoad2(c->llvm_builder, i64_type, compile_expr(c, binary->union_check, true), "");

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

        static_assert(COUNT_TOKENS == 78, "");
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

        static_assert(COUNT_TOKENS == 78, "");
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
                return LLVMBuildICmp(c->llvm_builder, op.i, value, LLVMConstNull(LLVMTypeOf(value)), "");
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

        static_assert(COUNT_TOKENS == 78, "");
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
            const size_t group_count = binary->lhs->type.kind == TYPE_GROUP ? binary->lhs->type.spec.group.count : 0;

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

    static_assert(COUNT_TOKENS == 78, "");
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

LLVMValueRef compile_expr_member(Compiler *c, Node_Member *member, bool ref) {
    Node *n = (Node *) member;
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
        member->method_receiver_llvm =
            LLVMBuildLoad2(c->llvm_builder, ptr_type, LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, 1, ""), "");

        LLVMValueRef impl =
            LLVMBuildLoad2(c->llvm_builder, ptr_type, LLVMBuildStructGEP2(c->llvm_builder, lhs_type, lhs, 2, ""), "");

        // Impl check
        if (c->optimization_level != O3) {
            LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

            LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntNE, impl, LLVMConstNull(ptr_type), "");
            LLVMBuildCondBr(c->llvm_builder, check, success, failure);

            // Failure
            LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
            compile_panic_v2(
                c, get_leftmost_point_of_node(n), CONTRACT_PANIC_NULL_TRAIT_METHOD_ACCESS, NULL, NULL, NULL);

            // Success
            LLVMPositionBuilderAtEnd(c->llvm_builder, success);
        }

        LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), member->trait_method, true)};

        return LLVMBuildLoad2(
            c->llvm_builder, ptr_type, LLVMBuildGEP2(c->llvm_builder, ptr_type, impl, indices, len(indices), ""), "");
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
        if (c->optimization_level != O3) {
            LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

            LLVMTypeRef i64 = LLVMInt64TypeInContext(c->llvm_context);
            if (member->lhs->type.kind == TYPE_TRAIT) {
                LLVMTypeRef  ptr_type = LLVMPointerTypeInContext(c->llvm_context, 0);
                LLVMValueRef actual = compile_load_if_not_null(c, lhs, ptr_type);
                LLVMValueRef expected = compile_type_info(c, &n->type);

                LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, actual, expected, "");
                LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                // Failure
                LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                compile_panic_v2(
                    c,
                    member->dot.pos,
                    CONTRACT_PANIC_TRAIT_TYPE_MISMATCH,
                    LLVMBuildPtrToInt(c->llvm_builder, actual, i64, ""),
                    LLVMBuildPtrToInt(c->llvm_builder, expected, i64, ""),
                    NULL);
            } else if (member->union_index) {
                LLVMValueRef actual = LLVMBuildLoad2(c->llvm_builder, i64, lhs, "");
                LLVMValueRef expected = LLVMConstInt(i64, member->union_index, true);

                LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, actual, expected, "");
                LLVMBuildCondBr(c->llvm_builder, check, success, failure);

                // Failure
                LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
                compile_panic_v2(
                    c,
                    member->dot.pos,
                    CONTRACT_PANIC_UNION_TYPE_MISMATCH,
                    actual,
                    expected,
                    LLVMBuildPtrToInt(c->llvm_builder, compile_type_info(c, &member->lhs->type), i64, ""));
            } else {
                unreachable();
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

LLVMValueRef compile_expr_interpolation(Compiler *c, Node_Interpolation *interpolation, bool ref) {
    Node *n = (Node *) interpolation;
    assert(!interpolation->is_constant);

    if (interpolation->do_not_allocate) {
        if (interpolation->children_count > 1) {
            LLVMValueRef marker = LLVMConstInt(
                compile_type(c, &c->interpolation_marker_type),
                interpolation->children_count - 1, // Do not count the marker
                type_is_signed(c->interpolation_marker_type));

            LLVMValueRef memory = compile_alloca(c, compile_type(c, &c->any_type));
            LLVMBuildStore(c->llvm_builder, compile_type_info(c, &c->interpolation_marker_type), memory);
            LLVMBuildStore(
                c->llvm_builder, marker, LLVMBuildStructGEP2(c->llvm_builder, c->any_type.llvm, memory, 1, ""));

            da_push(&c->group_values, LLVMBuildLoad2(c->llvm_builder, c->any_type.llvm, memory, ""));
        }

        ll_foreach(it, &interpolation->children) {
            da_push(&c->group_values, compile_expr(c, it, false));
        }
        return NULL;
    }

    LLVMTypeRef  element_type = compile_type(c, &c->any_type);
    LLVMValueRef memory = compile_alloca(c, LLVMArrayType(element_type, interpolation->children_count));

    size_t iota = 0;
    ll_foreach(it, &interpolation->children) {
        LLVMValueRef value = compile_expr(c, it, false);
        LLVMValueRef indices[] = {LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), iota++, true)};
        LLVMValueRef ptr = LLVMBuildGEP2(c->llvm_builder, element_type, memory, indices, len(indices), "");
        LLVMBuildStore(c->llvm_builder, value, ptr);
    }

    LLVMValueRef slice = compile_alloca(c, c->llvm_slice_type);
    LLVMBuildStore(c->llvm_builder, memory, slice);
    LLVMBuildStore(
        c->llvm_builder,
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), interpolation->children_count, true),
        LLVMBuildStructGEP2(c->llvm_builder, c->llvm_slice_type, slice, 1, ""));

    LLVMValueRef value = LLVMBuildLoad2(c->llvm_builder, c->llvm_slice_type, slice, "");
    assert(type_kind_eq(n->type, TYPE_UNION));
    return compile_cast_to_union(c, n->type.llvm, 1 + interpolation->is_constant, value, ref);
}

LLVMValueRef compile_expr_compound(Compiler *c, Node_Compound *compound, bool ref) {
    Node        *n = (Node *) compound;
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

LLVMValueRef compile_expr_call(Compiler *c, Node_Call *call, bool ref) {
    Node *n = (Node *) call;
    if (call->is_monomorphization_of_polymorphic_type) {
        return NULL;
    }

    if (call->is_type_cast) {
        Node *from = call->args.head;
        if (call->type_cast == TYPE_CAST_NOP) {
            return compile_expr(c, from, ref);
        }

        LLVMValueRef from_value = compile_expr(c, from, false);
        LLVMTypeRef  from_type = from->type.llvm;

        set_debug_pos(c, call->fn_source->token.pos);
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
    if (fn_spec->variadics_kind == VARIADICS_TYPED && !call->is_typed_variadics_direct) {
        Type *type = &fn_spec->args[fn_spec->variadics_index].type;
        assert(type->kind == TYPE_SLICE);
        variadics_type = compile_type(c, type->spec.slice.element);

        if (call->typed_variadics_count) {
            variadics_memory = compile_alloca(c, LLVMArrayType(variadics_type, call->typed_variadics_count));
        } else {
            variadics_memory = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }

        LLVMValueRef variadics_slice = compile_alloca(c, c->llvm_slice_type);
        LLVMBuildStore(c->llvm_builder, variadics_memory, variadics_slice);
        LLVMBuildStore(
            c->llvm_builder,
            LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), call->typed_variadics_count, true),
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

        if (arg->kind == NODE_UNARY && arg->token.kind == TOKEN_SPREAD) {
            Node_Unary *unary = (Node_Unary *) arg;

            LLVMValueRef expr = compile_expr(c, unary->value, false);
            args[args_iota].type = &unary->value->type;
            args[args_iota].value = expr;
            args_iota++;
            continue;
        }

        const size_t group_values_count_save = c->group_values.count;

        LLVMValueRef expr = compile_expr(c, arg, false);
        if (c->group_values.count == group_values_count_save) {
            Typed_LLVM_Value tv = {0};
            tv.type = &arg->type;
            tv.value = expr;
            if (variadics_memory && args_iota >= fn_spec->variadics_index) {
                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), args_iota - fn_spec->variadics_index, true),
                };

                LLVMValueRef dst =
                    LLVMBuildGEP2(c->llvm_builder, variadics_type, variadics_memory, indices, len(indices), "");
                LLVMBuildStore(c->llvm_builder, tv.value, dst);
            } else {
                args[args_iota] = tv;
            }
            args_iota++;
        } else {
            Type  *types = NULL;
            size_t count = c->group_values.count - group_values_count_save;
            if (arg->type.kind == TYPE_GROUP) {
                Type_Group *group = &arg->type.spec.group;
                types = group->data;
                assert(count == group->count);
            } else {
                assert(arg->kind == NODE_INTERPOLATION);
                Node_Interpolation *interpolation = (Node_Interpolation *) arg;
                assert(count == interpolation->children_count);
            }

            for (size_t i = 0; i < count; i++) {
                Typed_LLVM_Value tv = {0};
                tv.type = types ? &types[i] : &c->any_type;
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
        }

        c->group_values.count = group_values_count_save;
    }

    compile_optional_arguments(c, args, fn_spec, get_leftmost_point_of_node((Node *) call));

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

LLVMValueRef compile_expr_index(Compiler *c, Node_Index *index, bool ref) {
    Node *n = (Node *) index;
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

        compile_optional_arguments(c, args, fn_spec, get_leftmost_point_of_node(n));
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

    if (index->lhs->type.ref) {
        element_type = n->type.spec.slice.element;
    } else {
        static_assert(COUNT_TYPES == 27, "");
        switch (index->lhs->type.kind) {
        case TYPE_ARRAY:
            element_type = index->lhs->type.spec.array.element;
            break;

        case TYPE_DYNAMIC_ARRAY:
            element_type = index->lhs->type.spec.dynamic_array.element;
            break;

        case TYPE_SLICE:
            element_type = index->lhs->type.spec.slice.element;
            break;

        case TYPE_STRING:
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
        } else if (
            index->lhs->type.kind == TYPE_DYNAMIC_ARRAY ||                               //
            index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) //
        {
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
        if (c->optimization_level != O3) {
            LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

            LLVMValueRef check = LLVMBuildICmp(c->llvm_builder, LLVMIntSLE, a, b, "");
            LLVMBuildCondBr(c->llvm_builder, check, success, failure);

            // Failure
            LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
            compile_panic_v2(c, n->token.pos, CONTRACT_PANIC_RANGE_BEGIN_MORE_THAN_END, a, b, NULL);

            // Success
            LLVMPositionBuilderAtEnd(c->llvm_builder, success);
        }

        if (count) {
            // Bounds check
            if (c->optimization_level != O3) {
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
                compile_panic_v2(c, n->token.pos, CONTRACT_PANIC_RANGE_OUT_OF_BOUNDS, a, b, count);

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
    if (c->optimization_level != O3) {
        LLVMValueRef count = NULL;
        if (index->lhs->type.kind == TYPE_ARRAY) {
            count = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), index->lhs->type.spec.array.count, true);
        } else if (
            index->lhs->type.kind == TYPE_DYNAMIC_ARRAY ||                               //
            index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) //
        {
            count = LLVMBuildStructGEP2(c->llvm_builder, index->lhs->type.llvm, lhs, 1, "");
            count = LLVMBuildLoad2(c->llvm_builder, LLVMInt64TypeInContext(c->llvm_context), count, "");
        } else {
            unreachable();
        }

        LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
        LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        LLVMValueRef check_begin_of_a = LLVMBuildICmp(c->llvm_builder, LLVMIntSGE, a, LLVMConstNull(LLVMTypeOf(a)), "");
        LLVMValueRef check_end_of_a = LLVMBuildICmp(c->llvm_builder, LLVMIntSLT, a, count, "");
        LLVMValueRef check = LLVMBuildAnd(c->llvm_builder, check_begin_of_a, check_end_of_a, "");

        LLVMBuildCondBr(c->llvm_builder, check, success, failure);

        // Failure
        LLVMPositionBuilderAtEnd(c->llvm_builder, failure);
        compile_panic_v2(c, n->token.pos, CONTRACT_PANIC_INDEX_OUT_OF_BOUNDS, a, count, NULL);

        // Success
        LLVMPositionBuilderAtEnd(c->llvm_builder, success);
    }

    LLVMValueRef ptr = NULL;
    if (index->lhs->type.kind == TYPE_ARRAY) {
        ptr = lhs;
    } else if (
        index->lhs->type.kind == TYPE_DYNAMIC_ARRAY ||                               //
        index->lhs->type.kind == TYPE_SLICE || index->lhs->type.kind == TYPE_STRING) //
    {
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

static_assert(COUNT_NODES == 29, "");
LLVMValueRef compile_expr_impl(Compiler *c, Node *n, bool ref) {
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
    case NODE_ATOM:
        return compile_expr_atom(c, (Node_Atom *) n, ref);

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

    case NODE_UNARY:
        return compile_expr_unary(c, (Node_Unary *) n, ref);

    case NODE_BINARY:
        return compile_expr_binary(c, (Node_Binary *) n);

    case NODE_MEMBER:
        return compile_expr_member(c, (Node_Member *) n, ref);

    case NODE_IMPORT:
        unreachable();

    case NODE_DISTINCT:
        unreachable();

    case NODE_POLYMORPH:
        unreachable();

    case NODE_INTERPOLATION:
        return compile_expr_interpolation(c, (Node_Interpolation *) n, ref);

    case NODE_FN:
        return compile_fn(c, (Node_Fn *) n);

    case NODE_COMPOUND:
        return compile_expr_compound(c, (Node_Compound *) n, ref);

    case NODE_CALL:
        return compile_expr_call(c, (Node_Call *) n, ref);

    case NODE_INDEX:
        return compile_expr_index(c, (Node_Index *) n, ref);

    case NODE_INDEXABLE:
        unreachable();

    default:
        unreachable();
        break;
    }
}

static LLVMValueRef compile_auto_cast(Compiler *c, Node *n, LLVMValueRef result, Auto_Cast *auto_cast, bool ref) {
    static_assert(COUNT_AUTO_CASTS == 5, "");
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

    case AUTO_CAST_DYNAMIC_ARRAY_TO_SLICE: {
        assert(auto_cast->from.kind == TYPE_DYNAMIC_ARRAY);
        LLVMValueRef slice = compile_alloca(c, c->llvm_slice_type);
        LLVMBuildStore(
            c->llvm_builder, LLVMBuildLoad2(c->llvm_builder, c->llvm_slice_type, undo_load(result), ""), slice);

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

LLVMValueRef compile_expr(Compiler *c, Node *n, bool ref) {
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
