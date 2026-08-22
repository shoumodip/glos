#include "error.h"
#include "basic.h"
#include "node.h"
#include "token.h"
#include <assert.h>
#include <stdarg.h>

static bool active;

static SV  view_sv;
static Pos view_pos;

static const char *view_error_begin;
static const char *view_error_end;

static void error_begin(Error_Kind kind) {
    assert(!active);
    active = true;
    if (view_pos.path) {
        afprintf(stderr, ANSI_BOLD | ANSI_UNDERLINE, Pos_Fmt, Pos_Arg(view_pos));
        fprintf(stderr, " ");
    }

    // TODO: Do not print all caps
    switch (kind) {
    case EK_ERROR:
        afprintf(stderr, ANSI_COLOR_RED | ANSI_BOLD, "ERROR:");
        break;

    case EK_NOTE:
        afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "NOTE:");
        break;

    case EK_BLANK:
        return;
    }

    fprintf(stderr, " ");
}

typedef struct {
    Token begin;
    Token end;
} Range;

static void range_apply_token(Range *r, Token t) {
    if (t.sv.data < r->begin.sv.data) {
        r->begin = t;
    }

    if (t.sv.data + t.sv.count > r->end.sv.data + r->end.sv.count) {
        r->end = t;
    }
}

static_assert(COUNT_NODES == 29, "");
static void range_apply_node(Range *r, const Node *n) {
    if (!n) {
        return;
    }

    range_apply_token(r, n->token);
    switch (n->kind) {
    case NODE_ATOM:
        // Pass
        break;

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;
        range_apply_node(r, group->nodes.head);
        range_apply_node(r, group->nodes.tail);
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        range_apply_node(r, unary->value);
        if (n->token.kind == TOKEN_SIZEOF || n->token.kind == TOKEN_TYPEOF) {
            range_apply_token(r, unary->end);
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        range_apply_node(r, binary->lhs);
        range_apply_node(r, binary->rhs);
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        range_apply_node(r, member->lhs);
        if (member->rhs) {
            range_apply_token(r, member->rhs_end);
        }
    } break;

    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        range_apply_token(r, assertt->end);
    } break;

    case NODE_IMPORT: {
        Node_Import *import = (Node_Import *) n;
        range_apply_token(r, import->path);
    } break;

    case NODE_POLYMORPH: {
        Node_Polymorph *polymorph = (Node_Polymorph *) n;
        range_apply_node(r, (Node *) polymorph->name);
    } break;

    case NODE_DISTINCT: {
        Node_Distinct *distinct = (Node_Distinct *) n;
        range_apply_node(r, distinct->value);
    } break;

    case NODE_INTERPOLATION: {
        Node_Interpolation *interpolation = (Node_Interpolation *) n;
        range_apply_node(r, interpolation->children.head);
        range_apply_node(r, interpolation->children.tail);
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        range_apply_token(r, fn->args_end_token);
        range_apply_token(r, fn->returns_end_token);
        range_apply_node(r, fn->returns.tail);
        range_apply_node(r, fn->body);
    } break;

    case NODE_ENUM: {
        Node_Enum *enumm = (Node_Enum *) n;
        range_apply_token(r, enumm->end);
    } break;

    case NODE_TRAIT: {
        Node_Trait *trait = (Node_Trait *) n;
        range_apply_token(r, trait->end);
    } break;

    case NODE_UNION: {
        Node_Union *unionn = (Node_Union *) n;
        range_apply_token(r, unionn->end);
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;
        range_apply_token(r, structt->end);
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        range_apply_node(r, compound->lhs);
        range_apply_token(r, compound->end);
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        range_apply_node(r, call->fn_source);
        range_apply_token(r, call->end);
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        range_apply_node(r, index->lhs);
        range_apply_token(r, index->end);
    } break;

    case NODE_INDEXABLE: {
        Node_Indexable *indexable = (Node_Indexable *) n;
        range_apply_node(r, indexable->element);
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        range_apply_node(r, define->name);
        range_apply_node(r, define->type);
        range_apply_node(r, define->expr);
        range_apply_node(r, (Node *) define->name_polymorph);
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        range_apply_token(r, block->end);
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        range_apply_node(r, iff->consequence);
        range_apply_node(r, iff->antecedence);
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
        range_apply_node(r, forr->body);
    } break;

    case NODE_CASE: {
        Node_Case *casee = (Node_Case *) n;
        range_apply_node(r, casee->preds.tail);
    } break;

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        range_apply_token(r, sw->end);
    } break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        range_apply_node(r, defer->stmt);
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;
        range_apply_node(r, returnn->value);
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        range_apply_token(r, externn->end);
    } break;

    default:
        unreachable();
    }
}

// For multiline tokens
static const char *get_end_from_parts(SV sv, Pos pos) {
    const char *line_end = pos.line.data + pos.line.count;
    const char *sv_end = sv.data + sv.count;
    if (sv_end > line_end) {
        while (*sv_end && *sv_end != '\n') sv_end++;
        return sv_end;
    }
    return line_end;
}

Pos get_leftmost_point_of_node(const Node *n) {
    Range r = {.begin = n->token, .end = n->token};
    range_apply_node(&r, n);
    return r.begin.pos;
}

void error_node_begin(Error_Kind kind, const Node *n) {
    Range r = {.begin = n->token, .end = n->token};
    range_apply_node(&r, n);

    view_error_begin = r.begin.sv.data;
    view_error_end = r.end.sv.data + r.end.sv.count;

    view_sv.data = r.begin.pos.line.data;
    view_sv.count = get_end_from_parts(r.end.sv, r.end.pos) - view_sv.data;
    view_pos = r.begin.pos;
    error_begin(kind);
}

void error_parts_begin(Error_Kind kind, SV sv, Pos pos) {
    view_sv = pos.line;
    view_pos = pos;

    view_error_begin = sv.data;
    view_error_end = sv.data + sv.count;

    if (sv.count) {
        view_sv.count = get_end_from_parts(sv, pos) - view_sv.data;
    } else {
        view_sv = (SV) {0};
    }

    error_begin(kind);
}

void error_range_begin(Error_Kind kind, Pos begin, Pos end) {
    view_error_begin = begin.line.data + begin.col;
    view_error_end = end.line.data + end.col;

    view_sv.data = begin.line.data;
    view_sv.count = (end.line.data + end.line.count) - (begin.line.data);
    view_pos = begin;

    error_begin(kind);
}

void error_token_begin(Error_Kind kind, Token token) {
    error_parts_begin(kind, token.sv, token.pos);
}

void error_token_range_begin(Error_Kind kind, Token begin, Token end) {
    Range r = {.begin = begin, .end = end};
    view_error_begin = r.begin.sv.data;
    view_error_end = r.end.sv.data + r.end.sv.count;

    view_sv.data = r.begin.pos.line.data;
    view_sv.count = get_end_from_parts(r.end.sv, r.end.pos) - view_sv.data;
    view_pos = r.begin.pos;
    error_begin(kind);
}

void error_standalone_begin(Error_Kind kind) {
    error_begin(kind);
}

void error_finalize(void) {
    assert(active);
    active = false;
    fprintf(stderr, "\n");

    bool has = false;
    bool error = false;

    size_t trim = 0;
    {
        SV indent_min = {0};
        SV copy = view_sv;
        for (size_t lines = 0; copy.count; lines++) {
            SV line = sv_split_mut(&copy, '\n');
            if (lines > 5 && copy.count) {
                continue;
            }

            if (lines != 5 || !copy.count) {
                SV indent = sv_drop_while_mut(&line, is_space);
                if (!lines || sv_has_prefix(indent_min, indent)) {
                    indent_min = indent;
                }
            }
        }

        trim = indent_min.count;
        trim -= trim % 4;
    }

    for (size_t lines = 0; view_sv.count; lines++) {
        if (!has) {
            has = true;
            fprintf(stderr, "\n");
        }

        SV line = sv_split_mut(&view_sv, '\n');
        sv_drop_mut(&line, trim);

        if (lines > 5 && view_sv.count) {
            continue;
        }
        afprintf(stderr, ANSI_COLOR_CYAN, "    ");

        if (lines == 5 && view_sv.count) {
            afprintf(stderr, ANSI_COLOR_CYAN, "...\n");
            continue;
        }

        if (view_error_begin >= line.data && view_error_begin <= line.data + line.count) {
            const SV before = sv_drop_mut(&line, view_error_begin - line.data);
            ansi_set(stderr, ANSI_COLOR_CYAN);
            for (size_t i = 0; i < before.count; i++) {
                print_char_safe(stderr, before.data[i]);
            }
            ansi_reset(stderr);
            error = true;
        }

        if (view_error_end >= line.data && view_error_end <= line.data + line.count) {
            const SV before = sv_drop_mut(&line, view_error_end - line.data);
            ansi_set(stderr, ANSI_COLOR_RED | ANSI_BOLD);
            for (size_t i = 0; i < before.count; i++) {
                print_char_safe(stderr, before.data[i]);
            }
            ansi_reset(stderr);
            error = false;
        }

        ansi_set(stderr, error ? (ANSI_COLOR_RED | ANSI_BOLD) : ANSI_COLOR_CYAN);
        for (size_t i = 0; i < line.count; i++) {
            print_char_safe(stderr, line.data[i]);
        }
        print_char_safe(stderr, '\n');
        ansi_reset(stderr);
    }

    if (has) {
        fprintf(stderr, "\n");
    }

    view_pos = (Pos) {0};
    view_error_begin = NULL;
    view_error_end = NULL;
}

void error_node(Error_Kind kind, const Node *n, const char *fmt, ...) {
    error_node_begin(kind, n);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    error_finalize();
}

void error_parts(Error_Kind kind, SV sv, Pos pos, const char *fmt, ...) {
    error_parts_begin(kind, sv, pos);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    error_finalize();
}

void error_range(Error_Kind kind, Pos begin, Pos end, const char *fmt, ...) {
    error_range_begin(kind, begin, end);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    error_finalize();
}

void error_token(Error_Kind kind, Token token, const char *fmt, ...) {
    error_token_begin(kind, token);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    error_finalize();
}

void error_token_range(Error_Kind kind, Token begin, Token end, const char *fmt, ...) {
    error_token_range_begin(kind, begin, end);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    error_finalize();
}

void error_standalone(Error_Kind kind, const char *fmt, ...) {
    error_standalone_begin(kind);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    error_finalize();
}

/* NOTES:
 *
 * - For function without body, highlight the signature only
 * - For A[B] without A, highlight the B only
 *
 */
