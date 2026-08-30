#include "../error.h"
#include "checker.h"

static_assert(COUNT_NODES == 30, "");
bool loop_breaks(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (loop_breaks(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            return loop_breaks(iff->compile_time_real);
        }
        return loop_breaks(iff->consequence) || loop_breaks(iff->antecedence);
    }

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        return loop_breaks(case_->body);
    }

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            if (!sw->compile_time_real) {
                return false;
            }
            return loop_breaks(sw->compile_time_real->body);
        }

        for (Node *it = sw->cases.head; it; it = it->next) {
            if (!loop_breaks(it)) {
                return false;
            }
        }
        return sw->fallback != NULL;
    }

    case NODE_JUMP:
        return n->token.kind == TOKEN_BREAK;

    default:
        return false;
    }
}

static_assert(COUNT_NODES == 30, "");
bool always_returns(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        return is_atom_false(assertt->expr);
    }

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (always_returns(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            return always_returns(iff->compile_time_real);
        }

        if (is_atom_true(iff->condition)) {
            return always_returns(iff->consequence);
        }

        if (is_atom_false(iff->condition)) {
            return always_returns(iff->antecedence);
        }

        if (!iff->antecedence) {
            return false;
        }
        return always_returns(iff->consequence) && always_returns(iff->antecedence);
    }

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
        if (forr->init && always_returns(forr->init)) {
            return true;
        }

        bool infinite = false;
        if (!forr->condition) {
            infinite = true;
        } else if (is_atom_true(forr->condition)) {
            infinite = true;
        }

        if (infinite) {
            return !loop_breaks(forr->body);
        }

        return false;
    }

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        return always_returns(case_->body);
    }

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            if (!sw->compile_time_real) {
                return false;
            }
            return always_returns(sw->compile_time_real->body);
        }

        for (Node *it = sw->cases.head; it; it = it->next) {
            if (!always_returns(it)) {
                return false;
            }
        }

        if (sw->unionn || sw->enumeration) {
            return true;
        }
        return sw->fallback != NULL;
    }

    case NODE_RETURN:
        return true;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        if (type_kind_eq(call->fn->type, TYPE_FN)) {
            return call->fn->type.spec.fn->is_noreturn;
        }
        return false;
    }

    default:
        return false;
    }
}

void check_switch_exhaustive(Compiler *c, Node_Switch *sw) {
    if (sw->enumeration) {
        if (sw->preds_count < sw->enumeration->values_count && !sw->fallback) {
            error_node(EK_ERROR, (Node *) sw, "This switch statement is not complete");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    The following enumeration values are not handled:\n");
            ll_foreach(it, &sw->enumeration->values) {
                bool handled = false;
                for (size_t i = 0; i < sw->preds_count; i++) {
                    const Const_Value *pred_value = &sw->preds[i].value;
                    assert(pred_value->kind == CONST_VALUE_INT);
                    if (int128_eq(pred_value->as.integer, int128_from_u64(it->token.as.integer))) {
                        handled = true;
                        break;
                    }
                }

                if (!handled) {
                    afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "        - " SV_Fmt "\n", SV_Arg(it->token.sv));
                }
            }
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "\n");
            error_node(EK_NOTE, (Node *) sw->enumeration, "Enumeration defined here");
            exit(c, 1);
        }
    } else if (sw->unionn) {
        if (sw->preds_count < sw->unionn->variants_count + 1 && !sw->fallback) {
            error_node(EK_ERROR, (Node *) sw, "This switch statement is not complete");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    The following union variants are not handled:\n");

            const size_t variants_count = sw->unionn->variants_count + 1;

            bool *handled = arena_alloc(&temp_arena, variants_count * sizeof(*handled));
            for (size_t i = 0; i < sw->preds_count; i++) {
                const Const_Value *pred_value = &sw->preds[i].value;
                assert(pred_value->kind == CONST_VALUE_INT);

                const size_t pred_index = pred_value->as.integer.low;
                assert(pred_index < variants_count);
                handled[pred_index] = true;
            }

            for (size_t i = 0; i < variants_count; i++) {
                if (!handled[i]) {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "        - %s\n",
                        i ? type_to_cstr_raw(sw->unionn->node.type.spec.unionn->variants[i - 1].type) : "null");
                }
            }
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "\n");

            error_node(EK_NOTE, (Node *) sw->unionn, "Union defined here");
            exit(c, 1);
        }
    }
}
