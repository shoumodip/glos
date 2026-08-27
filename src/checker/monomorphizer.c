#include "../error.h"
#include "checker.h"

// #define MONOMORPHIZATION_LOG

void show_current_monomorphization(Compiler *c) {
    if (c->monomorphizing_site.expr) {
        error_node(EK_NOTE, c->monomorphizing_site.expr, "While attempting to monomorphize this");
        if (type_kind_eq(c->monomorphizing_site.node->type, TYPE_FN)) {
            show_note_about_the_function_being_called(
                c->monomorphizing_site.node,
                c->monomorphizing_site.is_method,
                c->monomorphizing_site.node->type.spec.fn);
        } else if (type_meta_kind_eq(c->monomorphizing_site.node->type, TYPE_TRAIT)) {
            // Pass
        } else if (type_meta_kind_eq(c->monomorphizing_site.node->type, TYPE_STRUCT)) {
            Type_Struct *spec = c->monomorphizing_site.node->type.spec.structt;
            Node_Struct *structure = spec->original_definition;
            error_node_begin(EK_NOTE, (Node *) structure);
            fprintf(stderr, "The structure being monomorphized has signature '(");

            for (size_t i = 0; i < spec->polymorphs_count; i++) {
                if (i) {
                    fprintf(stderr, ", ");
                }

                fprintf(
                    stderr,
                    "$" SV_Fmt ": %s",
                    SV_Arg(spec->polymorphs[i]->name->node.token.sv),
                    type_to_cstr_raw(spec->polymorphs[i]->node.type));
            }

            fprintf(stderr, ")'");
            error_finalize();
        } else if (c->monomorphizing_site.node->kind == NODE_IMPL) {
            // Pass
        } else {
            unreachable();
        }
    }

    for (size_t i = 0; i < c->monomorphization_stack.count; i++) {
        const Monomorphization m = c->monomorphization_stack.data[c->monomorphization_stack.count - i - 1];
        if (m.into->kind == NODE_FN) {
            for (Context_Fn *context = c->context.fn; context; context = context->outer) {
                if (context->fn == (Node_Fn *) m.into) {
                    error_node(EK_NOTE, m.site, "While inside this monomorphization");

                    ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
                    fprintf(stderr, "    Here are the polymorphic parameters used:\n\n");
                    ll_foreach(it, &context->fn->monomorphs) {
                        assert(it->is_monomorphized);
                        fprintf(stderr, "        " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                        const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                        fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "\n");
                    ansi_reset(stderr);

                    Node_Fn *from = get_function_literal(m.from);
                    assert(from); // It's polymorphic

                    Node *from_body_save = from->body;
                    from->body = NULL;
                    error_node(
                        EK_NOTE,
                        (Node *) from,
                        "Here is the %s that was monomorphized",
                        from->is_method ? "method" : "function");
                    from->body = from_body_save;
                }
            }
        } else if (m.into->kind == NODE_TRAIT) {
            error_node(EK_NOTE, m.site, "While inside this monomorphization");

            ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
            fprintf(stderr, "    Here are the polymorphic parameters used:\n\n");

            Node_Trait *trait = (Node_Trait *) m.into;
            Polymorphs *ps = trait->monomorphs.count ? &trait->monomorphs : &trait->polymorphs;
            ll_foreach(it, ps) {
                assert(it->is_monomorphized);
                fprintf(stderr, "        " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }

            fprintf(stderr, "\n");
            ansi_reset(stderr);

            assert(type_meta_kind_eq(m.from->type, TYPE_TRAIT));
            error_node(
                EK_NOTE, (Node *) m.from->type.spec.trait->definition, "Here is the trait that was monomorphized");
        } else if (m.into->kind == NODE_STRUCT) {
            error_node(EK_NOTE, m.site, "While inside this monomorphization");

            ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
            fprintf(stderr, "    Here are the polymorphic parameters used:\n\n");

            Node_Struct *structt = (Node_Struct *) m.into;
            Polymorphs  *ps = structt->monomorphs.count ? &structt->monomorphs : &structt->polymorphs;
            ll_foreach(it, ps) {
                assert(it->is_monomorphized);
                fprintf(stderr, "        " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }

            fprintf(stderr, "\n");
            ansi_reset(stderr);

            assert(type_meta_kind_eq(m.from->type, TYPE_STRUCT));
            error_node(
                EK_NOTE,
                (Node *) m.from->type.spec.structt->definition,
                "Here is the structure that was monomorphized");
        } else if (m.into->kind == NODE_IMPL) {
            error_node(EK_NOTE, m.site, "While inside this monomorphization");

            ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
            fprintf(stderr, "    Here are the polymorphic parameters used:\n\n");

            Node_Impl  *impl = (Node_Impl *) m.into;
            Polymorphs *ps = impl->monomorphs.count ? &impl->monomorphs : &impl->polymorphs;
            ll_foreach(it, ps) {
                assert(it->is_monomorphized);
                fprintf(stderr, "        " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }

            fprintf(stderr, "\n");
            ansi_reset(stderr);
        } else {
            unreachable();
        }
    }
}

void add_monomorph_parameter(
    Compiler *c, Node_Polymorph *polymorph, Type type, Const_Value value, Node_Polymorph *to_polymorph) //
{
    if (value.kind == CONST_VALUE_POLYMORPH && value.as.polymorph.polymorph->is_monomorphized) {
        type = value.as.polymorph.polymorph->monomorphization_type;
        value = value.as.polymorph.polymorph->monomorphization_value;
    }

    for (size_t i = c->monomorph_parameters.begin; i < c->monomorph_parameters.count; i++) {
        if (c->monomorph_parameters.data[i].from == polymorph) {
            return;
        }
    }

#ifdef MONOMORPHIZATION_LOG
    ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
    fprintf(
        stderr,
        "Add monomorph parameter (Currently %zu): " SV_Fmt " :: ",
        c->monomorph_parameters.count - c->monomorph_parameters.begin,
        SV_Arg(polymorph->name->node.token.sv));
    const_value_debug(stderr, type, value);
    fprintf(stderr, "\n\n");
    ansi_reset(stderr);
#endif // MONOMORPHIZATION_LOG

    const Monomorph_Parameter mp = {
        .from = polymorph,
        .type = type,
        .value = value,
        .to_polymorph = to_polymorph,
    };

    da_push(&c->monomorph_parameters, mp);
}

void add_monomorph_parameter_default_value(
    Compiler       *c,
    Node_Polymorph *polymorph,
    Type            type,
    Const_Value    *default_value,
    Node           *default_value_as_caller_location) //
{
    if (default_value) {
        add_monomorph_parameter(c, polymorph, type, *default_value, NULL);
    } else if (default_value_as_caller_location) {
        Const_Value location = default_const_value(c, type);
        assert(location.kind == CONST_VALUE_STRUCT);

        Const_Value_Struct *structure = &location.as.structt;
        assert(structure->spec->fields_count == 3);

        const Pos pos = get_leftmost_pos_of_node(default_value_as_caller_location);
        structure->fields[0] = const_value_string(sv_from_cstr(pos.path));
        structure->fields[1] = const_value_u64(pos.row + 1);
        structure->fields[2] = const_value_u64(pos.col + 1);
        add_monomorph_parameter(c, polymorph, type, location, NULL);
    } else {
        unreachable();
    }
}

// The return type just indicates whether the inference was done against a known type.
// It DOES NOT INDICATE TYPE VALIDITY. That is the responsibility of the pre-monomorphization analysis.
static_assert(COUNT_TYPES == 30, "");
bool infer_monomorph_parameters(Compiler *c, Node *n, const Type *actual, const Type *expected) {
    if (actual->ref < expected->ref) {
        return true;
    }

    switch (expected->kind) {
    case TYPE_FN:
        if (type_kind_eq(*actual, expected->kind) && actual->ref == expected->ref) {
            const Type_Fn *as = actual->spec.fn;
            const Type_Fn *es = expected->spec.fn;
            if (as->args_count == es->args_count && as->returns_count == es->returns_count) {
                for (size_t i = 0; i < as->args_count; i++) {
                    if (!infer_monomorph_parameters(c, n, &as->args[i].type, &es->args[i].type)) {
                        return false;
                    }
                }

                for (size_t i = 0; i < as->returns_count; i++) {
                    if (!infer_monomorph_parameters(c, n, &as->returns[i], &es->returns[i])) {
                        return false;
                    }
                }
            }
        }
        break;

    case TYPE_STRUCT: {
        const Type_Struct *es = expected->spec.structt;

#ifdef MONOMORPHIZATION_LOG
        error_node(EK_NOTE, n, "Match %s against %s", type_to_cstr(*actual), type_to_cstr(*expected));
#endif // MONOMORPHIZATION_LOG

        if (es->polymorphs_count && type_kind_eq(*actual, TYPE_STRUCT)) {
            const Type_Struct *as = actual->spec.structt;

            if (as->definition->defined_as != es->definition->defined_as) {
                return true;
            }

            assert(!as->definition->polymorphs.count);
            if (as->definition->monomorphs.count != es->polymorphs_count) {
                return true;
            }

            size_t it_index = 0;
            ll_foreach(ap, &as->definition->monomorphs) {
                Node_Polymorph *ep = es->polymorphs[it_index++];
                if (ep->is_type) {
                    assert(ap->is_type);
                    const Type et = type_without_meta(ep->node.type);
                    const Type at = type_without_meta(ap->node.type);

#ifdef MONOMORPHIZATION_LOG
                    afprintf(
                        stderr,
                        ANSI_COLOR_BLUE | ANSI_BOLD,
                        "    Infer %s against %s\n\n",
                        type_to_cstr(at),
                        type_to_cstr(et));
#endif // MONOMORPHIZATION_LOG

                    if (!infer_monomorph_parameters(c, n, &at, &et)) {
                        return false;
                    }
                } else {
                    assert(!ap->is_type);
                    assert(type_meta_kind_eq(ep->node.type, TYPE_POLYMORPH));
                    const Type_Polymorph et = ep->node.type.spec.polymorph;
                    if (et.is_definition) {
                        if (!check_that_type_is_known_noexit(n)) {
                            return false;
                        }
                        finalize_untyped_type(c, n);

#ifdef MONOMORPHIZATION_LOG
                        ansi_set(stderr, ANSI_COLOR_BLUE | ANSI_BOLD);
                        fprintf(stderr, "    Infer %s to be ", type_to_cstr(type_without_meta(ep->node.type)));
                        const_value_debug(stderr, ap->monomorphization_type, ap->monomorphization_value);
                        fprintf(stderr, "\n\n");
                        ansi_reset(stderr);
#endif // MONOMORPHIZATION_LOG

                        add_monomorph_parameter(
                            c, et.definition, ap->monomorphization_type, ap->monomorphization_value, NULL);
                    }
                }
            }
        }
    } break;

    case TYPE_ARRAY:
        if (type_kind_eq(*actual, expected->kind) && actual->ref == expected->ref) {
            Type_Array as = actual->spec.array;
            Type_Array es = expected->spec.array;
            if (es.count_polymorph) {
#ifdef MONOMORPHIZATION_LOG
                ansi_set(stderr, ANSI_COLOR_BLUE | ANSI_BOLD);
                fprintf(
                    stderr,
                    "    Infer %s to be %zu\n\n",
                    type_to_cstr(type_without_meta(es.count_polymorph->node.type)),
                    as.count);
                ansi_reset(stderr);
#endif // MONOMORPHIZATION_LOG

                add_monomorph_parameter(
                    c, es.count_polymorph, (Type) {.kind = TYPE_S64}, const_value_u64(as.count), NULL);

                es.count = as.count;
            }

            if (as.count == es.count) {
                if (!infer_monomorph_parameters(c, n, as.element, es.element)) {
                    return false;
                }
            }
        }
        break;

    case TYPE_DYNAMIC_ARRAY:
        if (type_kind_eq(*actual, expected->kind) && actual->ref == expected->ref) {
            Type *ae = actual->spec.dynamic_array.element;
            Type *ee = expected->spec.dynamic_array.element;
            if (!infer_monomorph_parameters(c, n, ae, ee)) {
                return false;
            }
        }
        break;

    case TYPE_SLICE:
        if (actual->ref == expected->ref) {
            Type *element = NULL;
            if (type_kind_eq(*actual, TYPE_SLICE)) {
                element = actual->spec.slice.element;
            }

            if (type_kind_eq(*actual, TYPE_ARRAY)) {
                element = actual->spec.array.element;
            }

            if (element) {
                if (!infer_monomorph_parameters(c, n, element, expected->spec.slice.element)) {
                    return false;
                }
            }
        }
        break;

    case TYPE_POLYMORPH:
        if (expected->spec.polymorph.is_definition) {
            if (!check_that_type_is_known_noexit(n)) {
                return false;
            }
            finalize_untyped_type(c, n);

            if (actual->is_meta) {
                assert(n->type.is_meta && &n->type == actual);
                n->emit_type_info = arena_clone(&default_arena, &n->type, sizeof(n->type));
                n->emit_type_info->is_meta = false;
                n->type = c->type_info_pointer_type;
            }

            Node_Polymorph *polymorph = expected->spec.polymorph.definition;
            const Type      type = type_with_ref(*actual, actual->ref - expected->ref);

#ifdef MONOMORPHIZATION_LOG
            error_node(EK_NOTE, (Node *) polymorph, "Infer to be %s", type_to_cstr(type));
#endif // MONOMORPHIZATION_LOG

            add_monomorph_parameter(c, polymorph, type, const_value_type(type), NULL);
        }
        break;

    default:
        // Pass
        break;
    }

    return true;
}

// TODO: Use a custom hasher instead of just operating on the raw bytes
static u64 ht_hasheq_monomorph_spec(const void *va, const void *vb, size_t n) {
    unused(n);

    const Monomorph_Spec a = *(Monomorph_Spec *) va;
    if (vb) {
        const Monomorph_Spec b = *(Monomorph_Spec *) vb;
        if (a.from != b.from) {
            return false;
        }

        if (a.params_count != b.params_count) {
            return false;
        }

        for (size_t i = 0; i < a.params_count; i++) {
            if (!type_eq(a.param_types[i], b.param_types[i])) {
                return false;
            }

            if (!const_value_eq(a.param_values[i], b.param_values[i])) {
                return false;
            }
        }

        return true;
    }

    u64 hash = ht_hasheq_bytes(&a.from, NULL, sizeof(Node *));
    hash = ht_hash_combine(hash, ht_hasheq_bytes(&a.params_count, NULL, sizeof(a.params_count)));
    hash = ht_hash_combine(hash, ht_hasheq_bytes(a.param_types, NULL, a.params_count * sizeof(*a.param_types)));
    hash = ht_hash_combine(hash, ht_hasheq_bytes(a.param_values, NULL, a.params_count * sizeof(*a.param_values)));
    return hash;
}

static void monomorphize_nodes(Monomorph_Replacements *rs, Nodes *ns, bool first) {
    Nodes from = *ns;
    memset(ns, 0, sizeof(*ns));

    ll_foreach(it, &from) {
        monomorphize_node(rs, &it, first);
        nodes_push(ns, it);
    }

    if (ns->tail) {
        ns->tail->next = NULL;
    }
}

static void monomorphize_polymorphs(Monomorph_Replacements *rs, Polymorphs *ps, bool first) {
    Polymorphs from = *ps;
    memset(ps, 0, sizeof(*ps));

    ll_foreach(it, &from) {
        monomorphize_node(rs, (Node **) &it, first);
        polymorphs_push(ps, it);
    }

    if (ps->tail) {
        ps->tail->next = NULL;
    }
}

static void monomorphize_replace(Monomorph_Replacements *rs, Node **from) {
    if (!*from) {
        return;
    }

    Node **to = (Node **) ht_get(rs, *from);
    if (to) {
        *from = *to;
    }
}

static_assert(COUNT_NODES == 31, "");
void monomorphize_node(Monomorph_Replacements *rs, Node **np, bool first) {
    if (!*np) {
        return;
    }

    Node *n = *np;
    if (first) {
        Node *copy = arena_clone(&default_arena, n, node_size(n->kind));
        memset(&copy->type, 0, sizeof(copy->type));
        ht_set(rs, n, copy);
        n = copy;
    } else {
        monomorphize_replace(rs, np);
        n = *np;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;
        if (first) {
            if (atom->definition_spec) {
                atom->definition_spec =
                    arena_clone(&default_arena, atom->definition_spec, sizeof(*atom->definition_spec));
                atom->definition_spec->check_status = UNCHECKED;
                atom->definition_spec->fn_context = NULL;
            }
        } else {
            if (atom->definition_spec) {
                monomorphize_replace(rs, (Node **) &atom->definition_spec->static_var_fn);
                monomorphize_replace(rs, (Node **) &atom->definition_spec->definition_node);
                monomorphize_replace(rs, (Node **) &atom->definition_spec->assignment_node);
                monomorphize_replace(rs, (Node **) &atom->definition_spec->polymorph);
            }

            monomorphize_replace(rs, (Node **) &atom->definition);
            monomorphize_replace(rs, (Node **) &atom->polymorph);
        }
    } break;

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;
        monomorphize_nodes(rs, &group->nodes, first);
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        monomorphize_node(rs, &unary->value, first);
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        monomorphize_node(rs, &binary->lhs, first);
        monomorphize_node(rs, &binary->rhs, first);
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        monomorphize_node(rs, &member->lhs, first);
        monomorphize_node(rs, &member->rhs, first);
    } break;

    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        monomorphize_node(rs, &assertt->expr, first);
        monomorphize_node(rs, &assertt->message, first);
    } break;

    case NODE_IMPORT:
        // Pass
        break;

    case NODE_DISTINCT: {
        Node_Distinct *distinct = (Node_Distinct *) n;
        monomorphize_node(rs, &distinct->value, first);
        if (!first) {
            monomorphize_replace(rs, (Node **) &distinct->defined_as);
        }
    } break;

    case NODE_POLYMORPH: {
        Node_Polymorph *polymorph = (Node_Polymorph *) n;
        monomorphize_node(rs, (Node **) &polymorph->name, first);
    } break;

    case NODE_INTERPOLATION: {
        Node_Interpolation *interpolation = (Node_Interpolation *) n;
        monomorphize_nodes(rs, &interpolation->children, first);
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;

        // Arguments
        {
            Nodes from = fn->args;
            memset(&fn->args, 0, sizeof(fn->args));

            fn->args_count = 0;
            fn->args_count_min = 0;

            bool optional = false;
            ll_foreach(it, &from) {
                assert(it->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) it;
                monomorphize_node(rs, &it, first);
                if (define->name_polymorph && define->name_polymorph->is_monomorphized) {
                    if (first) {
                        nodes_push(&fn->args, it);
                    }
                } else {
                    nodes_push(&fn->args, it);
                    fn->args_count++;

                    if (define->has_spread || define->expr) {
                        optional = true;
                    }

                    if (!optional) {
                        fn->args_count_min++;
                    }
                }
            }

            if (fn->args.tail) {
                fn->args.tail->next = NULL;
            }
        }

        monomorphize_nodes(rs, &fn->returns, first);
        monomorphize_node(rs, &fn->body, first);

        if (!first) {
            monomorphize_polymorphs(rs, &fn->polymorphs, first);
            monomorphize_replace(rs, (Node **) &fn->outer_fn);
            monomorphize_replace(rs, (Node **) &fn->defined_as);
            monomorphize_replace(rs, (Node **) &fn->trait_method);
        }

        fn->checked_fully = false;
        fn->checked_signature = false;
    } break;

    case NODE_ENUM: {
        Node_Enum *enumm = (Node_Enum *) n;
        monomorphize_node(rs, &enumm->underlying, first);
        monomorphize_nodes(rs, &enumm->values, first);

        if (!first) {
            monomorphize_replace(rs, (Node **) &enumm->defined_as);
            monomorphize_replace(rs, (Node **) &enumm->defined_in);
        }
    } break;

    case NODE_TRAIT: {
        Node_Trait *trait = (Node_Trait *) n;
        monomorphize_nodes(rs, &trait->methods, first);
        monomorphize_polymorphs(rs, &trait->polymorphs, first);

        if (!first) {
            monomorphize_replace(rs, (Node **) &trait->defined_as);
            monomorphize_replace(rs, (Node **) &trait->defined_in);
        }
    } break;

    case NODE_UNION: {
        Node_Union *unionn = (Node_Union *) n;
        monomorphize_nodes(rs, &unionn->variants, first);

        if (!first) {
            monomorphize_replace(rs, (Node **) &unionn->defined_as);
            monomorphize_replace(rs, (Node **) &unionn->defined_in);
        }
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;
        monomorphize_nodes(rs, &structt->fields, first);
        monomorphize_polymorphs(rs, &structt->polymorphs, first);

        if (!first) {
            monomorphize_replace(rs, (Node **) &structt->defined_as);
            monomorphize_replace(rs, (Node **) &structt->defined_in);
        }
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        monomorphize_node(rs, &compound->lhs, first);
        monomorphize_nodes(rs, &compound->children, first);
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        monomorphize_node(rs, &call->fn_source, first);
        monomorphize_nodes(rs, &call->args, first);

        // The body of a polymorphic function is not checked directly. Only the monomorphized copy is checked.
        // Therefore there is no need to check 'call->fn'
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        monomorphize_node(rs, &index->lhs, first);
        monomorphize_node(rs, &index->a, first);
        monomorphize_node(rs, &index->b, first);
    } break;

    case NODE_INDEXABLE: {
        Node_Indexable *indexable = (Node_Indexable *) n;
        monomorphize_node(rs, &indexable->element, first);
        monomorphize_node(rs, &indexable->count, first);
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        if (define->name_polymorph) {
            monomorphize_node(rs, (Node **) &define->name_polymorph, first);
        } else {
            monomorphize_node(rs, &define->name, first);
        }

        monomorphize_node(rs, &define->expr, first);
        monomorphize_node(rs, &define->type, first);
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        monomorphize_nodes(rs, &block->body, first);
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        monomorphize_node(rs, &iff->condition, first);
        monomorphize_node(rs, &iff->consequence, first);
        monomorphize_node(rs, &iff->antecedence, first);
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
        monomorphize_node(rs, &forr->init, first);
        monomorphize_node(rs, &forr->condition, first);
        monomorphize_node(rs, &forr->update, first);
        monomorphize_node(rs, &forr->body, first);
    } break;

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        monomorphize_nodes(rs, &case_->preds, first);
        monomorphize_node(rs, &case_->body, first);
    } break;

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        monomorphize_node(rs, &sw->expr, first);
        monomorphize_nodes(rs, &sw->cases, first);

        if (!first) {
            monomorphize_replace(rs, &sw->fallback);
        }
    } break;

    case NODE_IMPL: {
        Node_Impl *impl = (Node_Impl *) n;
        monomorphize_node(rs, &impl->receiver, first);
        monomorphize_node(rs, &impl->trait, first);
        monomorphize_nodes(rs, &impl->methods, first); // TODO: Can we get away with not doing this??
        monomorphize_polymorphs(rs, &impl->polymorphs, first);
        impl->checked = false;
    } break;

    case NODE_SELF: {
        Node_Self *self = (Node_Self *) n;
        if (!first) {
            monomorphize_replace(rs, &self->definition);
        }
    } break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        monomorphize_node(rs, &defer->stmt, first);
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;
        monomorphize_node(rs, &returnn->value, first);
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        monomorphize_nodes(rs, &externn->nodes, first);
    } break;

    default:
        unreachable();
        break;
    }
}

static_assert(COUNT_TYPES == 30, "");
static bool type_is_polymorphic(Type type) {
    switch (type.kind) {
    case TYPE_FN: {
        const Type_Fn *spec = type.spec.fn;
        for (size_t i = 0; i < spec->args_count; i++) {
            if (type_is_polymorphic(spec->args[i].type)) {
                return true;
            }
        }

        return type_is_polymorphic(*spec->return_type);
    }

    case TYPE_TRAIT:
        return type.spec.trait->polymorph != NULL;

    case TYPE_STRUCT:
        return type.spec.structt->polymorphs_count != 0;

    case TYPE_ARRAY:
        return type.spec.array.count_polymorph != NULL || type_is_polymorphic(*type.spec.array.element);

    case TYPE_DYNAMIC_ARRAY:
        return type_is_polymorphic(*type.spec.dynamic_array.element);

    case TYPE_SLICE:
        return type_is_polymorphic(*type.spec.slice.element);

    case TYPE_POLYMORPH: {
        Node_Polymorph *polymorph = type.spec.polymorph.definition;
        if (!polymorph->is_monomorphized) {
            return true;
        }

        if (polymorph->is_type) {
            assert(polymorph->monomorphization_value.kind == CONST_VALUE_TYPE);
            return type_is_polymorphic(polymorph->monomorphization_value.as.type);
        }

        return type_is_polymorphic(polymorph->monomorphization_type);
    }

    case TYPE_GROUP: {
        const Type_Group *spec = &type.spec.group;
        for (size_t i = 0; i < spec->count; i++) {
            if (type_is_polymorphic(spec->data[i])) {
                return true;
            }
        }

        return false;
    }

    default:
        return false;
    }
}

Node *monomorphize(Compiler *c, Node *n, Node *site) {
    const Monomorphizing_Site monomorphizing_site_save = c->monomorphizing_site;
    memset(&c->monomorphizing_site, 0, sizeof(c->monomorphizing_site));

    const size_t ref = n->type.ref;

#ifdef MONOMORPHIZATION_LOG
    afprintf(stderr, ANSI_COLOR_BLUE | ANSI_BOLD, "Monomorphize {\n\n");
#endif // MONOMORPHIZATION_LOG

    Monomorphization monomorphization = {
        .from = n,
        .site = site,
        .site_fn = c->context.fn ? c->context.fn->fn : NULL,
    };

    const bool is_fn = type_kind_eq(n->type, TYPE_FN) || n->kind == NODE_FN;
    const bool is_trait = type_meta_kind_eq(n->type, TYPE_TRAIT);
    const bool is_struct = type_meta_kind_eq(n->type, TYPE_STRUCT);
    const bool is_impl = n->kind == NODE_IMPL;
    if (is_fn) {
        Node_Fn *fn = get_function_literal(n);
        assert(fn);
        n = (Node *) fn;
    } else if (is_trait) {
        n = (Node *) n->type.spec.trait->definition;
    } else if (is_struct) {
        n = (Node *) n->type.spec.structt->definition;
    } else if (is_impl) {
        // Pass
    } else {
        unreachable();
    }

    bool is_complete = true;
    if (is_trait || is_struct) {
        for (size_t i = c->monomorph_parameters.begin; i < c->monomorph_parameters.count; i++) {
            Monomorph_Parameter it = c->monomorph_parameters.data[i];
            if (it.to_polymorph && !it.to_polymorph->is_monomorphized) {
                is_complete = false;
                break;
            }

            if (it.value.kind == CONST_VALUE_POLYMORPH && !it.value.as.polymorph.polymorph->is_monomorphized) {
                is_complete = false;
                break;
            }

            if (it.value.kind == CONST_VALUE_TYPE && type_is_polymorphic(it.value.as.type)) {
                is_complete = false;
                break;
            }
        }
    }

    Monomorph_Spec spec = {0};
    if (is_complete) {
        spec.from = n;
        spec.params_count = c->monomorph_parameters.count - c->monomorph_parameters.begin;
        spec.param_types = arena_alloc(&default_arena, spec.params_count * sizeof(*spec.param_types));
        spec.param_values = arena_alloc(&default_arena, spec.params_count * sizeof(*spec.param_values));
        for (size_t i = 0; i < spec.params_count; i++) {
            Monomorph_Parameter it = c->monomorph_parameters.data[c->monomorph_parameters.begin + i];
            spec.param_types[i] = it.type;
            spec.param_values[i] = it.value;
        }

        if (!c->monomorph_intern.hasheq) {
            c->monomorph_intern.hasheq = ht_hasheq_monomorph_spec;
        }

        Node **into = ht_get(&c->monomorph_intern, spec);
        if (into) {
            arena_reset(&default_arena, spec.param_types);
            n = *into;
            monomorphization.into = n;
            goto end;
        }
    }

    for (size_t i = c->monomorph_parameters.begin; i < c->monomorph_parameters.count; i++) {
        Monomorph_Parameter it = c->monomorph_parameters.data[i];
        it.from->is_monomorphized = true;
        it.from->monomorphization_type = it.type;
        it.from->monomorphization_value = it.value;

        if (it.to_polymorph) {
            it.to_polymorph->monomorphization_type = it.type;
            it.to_polymorph->monomorphization_value = it.value;
        }
    }

#ifdef MONOMORPHIZATION_LOG
    if (is_fn) {
        Node_Fn *fn = (Node_Fn *) n;
        error_node(EK_NOTE, n, "Beginning to monomorphize this node");
        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        ll_foreach(it, &fn->polymorphs) {
            fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
            const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        ansi_reset(stderr);
    } else if (is_trait) {
        Node_Trait *trait = (Node_Trait *) n;
        error_node(EK_NOTE, n, "Beginning to monomorphize this node");
        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        ll_foreach(it, &trait->polymorphs) {
            fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
            const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        ansi_reset(stderr);
    } else if (is_struct) {
        Node_Struct *structt = (Node_Struct *) n;
        error_node(EK_NOTE, n, "Beginning to monomorphize this node");
        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        ll_foreach(it, &structt->polymorphs) {
            fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
            const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        ansi_reset(stderr);
    } else if (is_impl) {
        Node_Impl *impl = (Node_Impl *) n;
        error_node(EK_NOTE, n, "Beginning to monomorphize this node");
        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        ll_foreach(it, &impl->polymorphs) {
            fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
            const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        ansi_reset(stderr);
    } else {
        unreachable();
    }
#endif // MONOMORPHIZATION_LOG

    monomorphize_node(&c->monomorph_replacements, &n, true);
    monomorphize_node(&c->monomorph_replacements, &n, false);

    for (size_t i = c->monomorph_parameters.begin; i < c->monomorph_parameters.count; i++) {
        Node_Polymorph *it = c->monomorph_parameters.data[i].from;
        it->is_monomorphized = false;
        memset(&it->monomorphization_type, 0, sizeof(it->monomorphization_type));
        memset(&it->monomorphization_value, 0, sizeof(it->monomorphization_value));
    }

    if (is_fn) {
        assert(is_complete);
        Node_Fn *fn = (Node_Fn *) n;
        fn->monomorphs = fn->polymorphs;
        memset(&fn->polymorphs, 0, sizeof(fn->polymorphs));

#ifdef MONOMORPHIZATION_LOG
        error_node(EK_NOTE, n, "Monomorphized this node");
        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        ll_foreach(it, &fn->monomorphs) {
            fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
            const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        ansi_reset(stderr);
        show_current_monomorphization(c);
#endif // MONOMORPHIZATION_LOG

    } else if (is_trait) {
        Node_Trait *trait = (Node_Trait *) n;
        if (is_complete) {
            trait->monomorphs = trait->polymorphs;
            memset(&trait->polymorphs, 0, sizeof(trait->polymorphs));

#ifdef MONOMORPHIZATION_LOG
            error_node(EK_NOTE, n, "Monomorphized this node");
            ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
            ll_foreach(it, &trait->monomorphs) {
                fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "\n");
            ansi_reset(stderr);
            show_current_monomorphization(c);
#endif // MONOMORPHIZATION_LOG

        } else {
            ll_foreach(it, &trait->polymorphs) {
                it->node.type = it->monomorphization_type;
            }

#ifdef MONOMORPHIZATION_LOG
            error_node(EK_NOTE, n, "Monomorphized this node partially");
            ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
            ll_foreach(it, &trait->polymorphs) {
                fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "\n");
            ansi_reset(stderr);
            show_current_monomorphization(c);
#endif // MONOMORPHIZATION_LOG
        }
    } else if (is_struct) {
        Node_Struct *structt = (Node_Struct *) n;
        if (is_complete) {
            structt->monomorphs = structt->polymorphs;
            memset(&structt->polymorphs, 0, sizeof(structt->polymorphs));

#ifdef MONOMORPHIZATION_LOG
            error_node(EK_NOTE, n, "Monomorphized this node");
            ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
            ll_foreach(it, &structt->monomorphs) {
                fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "\n");
            ansi_reset(stderr);
            show_current_monomorphization(c);
#endif // MONOMORPHIZATION_LOG

        } else {
            ll_foreach(it, &structt->polymorphs) {
                it->node.type = it->monomorphization_type;
            }

#ifdef MONOMORPHIZATION_LOG
            error_node(EK_NOTE, n, "Monomorphized this node partially");
            ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
            ll_foreach(it, &structt->polymorphs) {
                fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
                const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "\n");
            ansi_reset(stderr);
            show_current_monomorphization(c);
#endif // MONOMORPHIZATION_LOG
        }
    } else if (is_impl) {
        assert(is_complete);
        Node_Impl *impl = (Node_Impl *) n;
        impl->monomorphs = impl->polymorphs;
        memset(&impl->polymorphs, 0, sizeof(impl->polymorphs));

#ifdef MONOMORPHIZATION_LOG
        error_node(EK_NOTE, n, "Monomorphized this node");
        ansi_set(stderr, ANSI_COLOR_MAGENTA | ANSI_BOLD);
        ll_foreach(it, &impl->monomorphs) {
            fprintf(stderr, "    " SV_Fmt " :: ", SV_Arg(it->name->node.token.sv));
            const_value_debug(stderr, it->monomorphization_type, it->monomorphization_value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        ansi_reset(stderr);
        show_current_monomorphization(c);
#endif // MONOMORPHIZATION_LOG

    } else {
        unreachable();
    }

    monomorphization.into = n;
    if (is_complete) {
        ht_set(&c->monomorph_intern, spec, monomorphization.into);
    }
    da_push(&c->monomorphization_stack, monomorphization);

    if (is_impl) {
        Node_Impl *impl = (Node_Impl *) n;
        check_expr(c, impl->receiver, REF_NONE);
        impl->receiver->type.is_meta = false;
        check_expr(c, impl->trait, REF_NONE);
        impl->trait->type.is_meta = false;
    } else {
        check_expr(c, n, REF_NONE);
    }
    c->monomorphization_stack.count--;

end:
    if (!is_impl) {
        n->type.ref = ref;
    }

    if (is_trait) {
        const Node *from = monomorphization.from;
        const Node *into = monomorphization.into;
        assert(type_meta_kind_eq(from->type, TYPE_TRAIT));
        assert(type_meta_kind_eq(into->type, TYPE_TRAIT));
        into->type.spec.trait->original_definition = from->type.spec.trait->original_definition;
    }

    if (is_struct) {
        const Node *from = monomorphization.from;
        const Node *into = monomorphization.into;
        assert(type_meta_kind_eq(from->type, TYPE_STRUCT));
        assert(type_meta_kind_eq(into->type, TYPE_STRUCT));
        into->type.spec.structt->original_definition = from->type.spec.structt->original_definition;
    }

#ifdef MONOMORPHIZATION_LOG
    if (!is_impl) {
        error_node(EK_NOTE, n, "Type checked to %s", type_to_cstr(type_without_meta(n->type)));
        show_current_monomorphization(c);
    }

    afprintf(stderr, ANSI_COLOR_BLUE | ANSI_BOLD, "}\n\n");
#endif // MONOMORPHIZATION_LOG

    c->monomorphizing_site = monomorphizing_site_save;
    c->monomorphizing_site.node = n;
    return n;
}
