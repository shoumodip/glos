#include "../error.h"
#include "compiler.h"

void compile_var_def(Compiler *c, Node_Atom *it) {
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
        sb_push_fn_name(&default_sb, it->definition_spec->static_var_fn, it->module);
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

void compile_local_var_debug(Compiler *c, Node_Atom *it, LLVMMetadataRef var_debug_type) {
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

void compile_defers(Compiler *c, size_t from, bool rollback) {
    for (size_t i = c->defers.count; i > from; i--) {
        compile_stmt(c, c->defers.data[i - 1]);
    }

    if (rollback) {
        c->defers.count = from;
    }
}

void compile_stmt_define(Compiler *c, Node_Define *define) {
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
}

void compile_stmt_block(Compiler *c, Node_Block *block) {
    Node *n = (Node *) block;

    const size_t    defers_count_save = c->defers.count;
    LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;
    c->llvm_debug_scope = LLVMDIBuilderCreateLexicalBlock(
        c->llvm_debug_builder,
        c->llvm_debug_scope,
        get_debug_file(c, n->token.pos.path),
        n->token.pos.row + 1,
        n->token.pos.col + 1);

    for (Node *it = block->body.head; it; it = it->next) {
        compile_stmt(c, it);
    }

    compile_defers(c, defers_count_save, true);
    c->llvm_debug_scope = llvm_debug_scope_save;
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

void compile_stmt_if(Compiler *c, Node_If *iff) {
    Node *n = (Node *) iff;
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
        introduce_ghost_for_union(c, iff->context_replace.to, iff->context_replace.from->node.type.kind == TYPE_TRAIT);
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
}

void compile_stmt_for(Compiler *c, Node_For *forr) {
    Node *n = (Node *) forr;

    LLVMMetadataRef llvm_debug_scope_save = c->llvm_debug_scope;
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
}

void compile_stmt_switch(Compiler *c, Node_Switch *sw) {
    Node *n = (Node *) sw;
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

    bool chain = false;
    if (sw->trait) {
        expr = undo_load(expr);
        expr = LLVMBuildLoad2(c->llvm_builder, ptr_type, expr, "");
        chain = true;
    } else if (sw->unionn) {
        expr = undo_load(expr);
        expr = LLVMBuildLoad2(c->llvm_builder, i64_type, expr, "");
    } else if (type_is_pointer(sw->expr->type) || sw->is_expr_type_info || sw->compare_overload) {
        chain = true;
    }

    if (chain) {
        LLVMBasicBlockRef end = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");

        size_t iota = 0;
        for (Node *it = sw->cases.head; it; it = it->next) {
            Node_Case *branch = (Node_Case *) it;
            if (!branch->preds.head) {
                continue; // Fallback
            }

            LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            for (Node *pred = branch->preds.head; pred; pred = pred->next) {
                set_debug_pos(c, pred->token.pos);

                LLVMValueRef value = compile_const_value(c, sw->preds[iota++].value, sw->expr->type);
                LLVMValueRef match = NULL;
                if (sw->compare_overload) {
                    const void *checkpoint = arena_alloc(&temp_arena, 0);

                    Typed_LLVM_Value fn = {0};
                    fn.value = compile_fn(c, sw->compare_overload);
                    fn.type = &sw->compare_overload->node.type;

                    const Type_Fn    *fn_spec = fn.type->spec.fn;
                    Typed_LLVM_Value *args = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*args));

                    args[0].value = expr;
                    args[0].type = &fn_spec->args[0].type;

                    LLVMValueRef expr_load_ptr = NULL;
                    if (type_is_compound(sw->expr->type)) {
                        expr_load_ptr = get_load_ptr(expr);
                        value = LLVMBuildLoad2(
                            c->llvm_builder, LLVMTypeOf(value), compile_const_value_into_memory(c, value), "");
                    }

                    args[1].value = value;
                    args[1].type = &fn_spec->args[1].type;

                    compile_optional_arguments(c, args, fn_spec, get_leftmost_point_of_node(pred));
                    LLVMValueRef result = compile_call(c, fn, args, fn_spec->args_count, false, false);

                    arena_reset(&temp_arena, checkpoint);
                    match = LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, result, LLVMConstNull(LLVMTypeOf(result)), "");

                    if (expr_load_ptr) {
                        // The call compilation erased this load
                        expr = LLVMBuildLoad2(c->llvm_builder, sw->expr->type.llvm, expr_load_ptr, "");
                    }
                } else {
                    match = LLVMBuildICmp(c->llvm_builder, LLVMIntEQ, expr, value, "");
                }

                LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
                LLVMBuildCondBr(c->llvm_builder, match, body, next);
                LLVMPositionBuilderAtEnd(c->llvm_builder, next);
            }

            LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(c->llvm_context, c->llvm_fn, "");
            LLVMBuildBr(c->llvm_builder, next);

            LLVMPositionBuilderAtEnd(c->llvm_builder, body);
            if (branch->context_replace.to) {
                introduce_ghost_for_union(
                    c, branch->context_replace.to, branch->context_replace.from->node.type.kind == TYPE_TRAIT);
            }
            compile_stmt(c, branch->body);
            LLVMBuildBr(c->llvm_builder, end);

            LLVMPositionBuilderAtEnd(c->llvm_builder, next);
        }

        if (sw->fallback) {
            compile_stmt(c, ((Node_Case *) sw->fallback)->body);
        }

        LLVMBuildBr(c->llvm_builder, end);
        LLVMPositionBuilderAtEnd(c->llvm_builder, end);
        return;
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

    bool jump_to_end = true;
    LLVMPositionBuilderAtEnd(c->llvm_builder, fallback);
    if (sw->fallback) {
        compile_stmt(c, ((Node_Case *) sw->fallback)->body);
    } else if (sw->enumeration) {
        if (c->optimization_level != O3) {
            set_debug_pos(c, n->token.pos);
            compile_panic_v2(
                c,
                n->token.pos,
                CONTRACT_PANIC_INVALID_ENUM_VALUE,
                expr,
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), type_is_signed(sw->expr->type), true),
                NULL);
            jump_to_end = false;
        }
    } else if (sw->unionn) {
        if (c->optimization_level != O3) {
            set_debug_pos(c, n->token.pos);
            compile_panic_v2(c, n->token.pos, CONTRACT_PANIC_INVALID_UNION_TAG, expr, NULL, NULL);
            jump_to_end = false;
        }
    }

    LLVMSetCurrentDebugLocation2(c->llvm_builder, NULL);
    if (jump_to_end) {
        LLVMBuildBr(c->llvm_builder, end);
    }
    LLVMPositionBuilderAtEnd(c->llvm_builder, end);
}

void compile_stmt_return(Compiler *c, Node_Return *returnn) {
    Node *n = (Node *) returnn;

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
}

static_assert(COUNT_NODES == 29, "");
void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT:
        // Pass
        break;

    case NODE_DEFINE:
        compile_stmt_define(c, (Node_Define *) n);
        break;

    case NODE_BLOCK:
        compile_stmt_block(c, (Node_Block *) n);
        break;

    case NODE_IF:
        compile_stmt_if(c, (Node_If *) n);
        break;

    case NODE_FOR:
        compile_stmt_for(c, (Node_For *) n);
        break;

    case NODE_CASE:
        unreachable();

    case NODE_SWITCH:
        compile_stmt_switch(c, (Node_Switch *) n);
        break;

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

    case NODE_RETURN:
        compile_stmt_return(c, (Node_Return *) n);
        break;

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
