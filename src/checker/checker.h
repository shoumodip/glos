#ifndef CHECKER_INTERNAL_H
#define CHECKER_INTERNAL_H

#include "../checker.h"

// Basics //////////////////////////////////////////////////////////////////////////////////////////
typedef enum {
    REF_NONE,
    REF_ADDR,
    REF_SLICE,
    REF_ASSIGN,

    REF_ADDR_MEMBER,
    REF_ASSIGN_MEMBER,
} Ref_Kind;

// Utilities ///////////////////////////////////////////////////////////////////////////////////////
const char *order_postfix(size_t n);
i64         i64_from_int128(Compiler *c, Node *n, Int128 x, bool min_zero, const char *label);

bool type_is_trait(Type type);
bool type_is_union(Type type);
bool type_eq_without_distinct(Type a, Type b);

bool is_atom_true(Node *n);
bool is_atom_false(Node *n);

bool node_is_null(Node *n);
bool is_node_caller_location(Node *n);
bool node_is_runtime_polymorphic_expression(Node *n);

void error_undefined(Compiler *c, const Token *t, const char *label, bool no_exit);
void error_redefinition(Compiler *c, const Node *n, const Pos *previous_pos);
void error_redefinition_add_helper_message_for_import(
    const Node *this, const Module *module, const Context *context, const char *label);
void error_redefinition_global(
    Compiler *c, const Node *this, const Node *previous, const Module *module, const Context *context);
void error_number_of_return_values_mismatch(Compiler *c, Token token, size_t expected, size_t actual);
void maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(Node *n, Type expected);

typedef struct {
    Int128 min;
    Int128 max;
} Int_Limit;

Int_Limit get_int_limit(Type type);
void      check_int_limit_ex(Compiler *c, Node *n, Int128 value, bool min_zero, const char *label);
void      check_int_limit(Compiler *c, Node *n, Int128 value);

bool     get_builtin_type_kind(SV name, Type_Kind *kind);
Int128   get_enum_value(Compiler *c, Node_Enum *enumm, SV name, const Token *t);
size_t   get_union_type_index(Compiler *c, Node *n, Type unionn);
Node    *get_node_from_group(Node *n, size_t index, i64 *group_index);
Node_Fn *get_function_literal(Node *fn);

void set_auto_cast(Compiler *c, Node *n, i64 index, Auto_Cast_Kind kind, Type from, Type to);
void cast_untyped(Compiler *c, Node *n, Type expected);
void finalize_untyped_type(Compiler *c, Node *n);

bool try_auto_cast_untyped(Compiler *c, Node *n, Type expected);
bool try_auto_cast_type_to_rtti(Compiler *c, Node *n, Type expected);
bool try_auto_cast_literal(Node *n, Type expected);
bool try_auto_cast(Compiler *c, Node *n, Type expected, i64 group_index);

void make_sure_import_is_ready(Compiler *c, Node_Import *import);

// Type Assertions /////////////////////////////////////////////////////////////////////////////////
bool check_that_type_is_known_noexit(const Node *n);
void check_that_type_is_known(Compiler *c, const Node *n);
bool type_assert_noexit(Compiler *c, Node *n, Type expected);
Type type_assert(Compiler *c, Node *n, Type expected);
bool type_assert_grouped_noexit(Compiler *c, Node *n, Type expected, i64 group_index, Token *requirement);
Type type_assert_grouped(Compiler *c, Node *n, Type expected, i64 group_index, Token *requirement);
Type type_assert_node(Compiler *c, Node *a, Node *b);
Type type_assert_numeric(Compiler *c, const Node *n, bool pointers_allowed, bool floats_allowed);
Type type_assert_scalar(Compiler *c, const Node *n);
bool type_assert_type_noexit(const Node *n);
Type type_assert_type(Compiler *c, const Node *n);
void type_assert_type_or_Type(Compiler *c, const Node *n);

Type_Trait_Impl *check_type_satisfies_trait(Compiler *c, Type type, Type_Trait *trait, Node *n, i64 group_index);

// Constant Expressions ////////////////////////////////////////////////////////////////////////////
Const_Value default_const_value(Compiler *c, Type type);
Const_Value const_value_to_trait(Node *n, Type *type, Type_Trait_Impl *impl, Const_Value value);
Const_Value const_value_to_union(Type union_type, size_t union_index, Const_Value value);

bool        eval_const_binary_equality(Compiler *c, Node_Binary *binary);
Const_Value const_value_of_var(Compiler *c, Node_Atom *var);

Const_Value eval_const_expr_atom(Compiler *c, Node_Atom *atom, bool ref);
Const_Value eval_const_expr_unary(Compiler *c, Node_Unary *unary);
Const_Value eval_const_expr_binary(Compiler *c, Node_Binary *binary);
Const_Value eval_const_expr_member(Compiler *c, Node_Member *member);
Const_Value eval_const_expr_interpolation(Compiler *c, Node_Interpolation *interpolation);
Const_Value eval_const_expr_compound(Compiler *c, Node_Compound *compound);
Const_Value eval_const_expr_call(Compiler *c, Node_Call *call);
Const_Value eval_const_expr_index(Compiler *c, Node_Index *index);
Const_Value eval_const_expr_impl(Compiler *c, Node *n, bool ref);
Const_Value eval_const_expr(Compiler *c, Node *n, bool ref);

// Definitions /////////////////////////////////////////////////////////////////////////////////////
Node_Atom *module_globals_find_ex(Compiler *c, Module *m, SV name, Module *skip);
Node_Atom *module_globals_find(Compiler *c, Module *m, SV name);
Node_Fn   *get_main(Compiler *c);

void define_orderless_node(Compiler *c, Node *n, const size_t block_start);
void define_orderless_nodes_of_module(Compiler *c, Module *module, const Token *unqualified_import_token);

void push_context_replace(Compiler *c, Context_Replace *replace, Node_Atom *from, Type to);
void check_definition(Compiler *c, Node_Atom *it, Node *it_expr, Node *type, bool called_from_if_needed);
void check_definition_if_needed(Compiler *c, Node_Atom *definition, Node *usage, Ref_Kind ref);
void check_ident(Compiler *c, Node *n, Ref_Kind ref);

// Control Flow Analysis ///////////////////////////////////////////////////////////////////////////
bool loop_breaks(Node *n);
bool always_returns(Node *n);

void check_switch_exhaustive(Compiler *c, Node_Switch *sw);

// Calls ///////////////////////////////////////////////////////////////////////////////////////////
typedef struct {
#define in
#define out
#define inout

    // The whole expression
    in Node *expr;

    // In case of a method
    in Node *receiver;

    inout Node *fn;        // The actual function, might differ in case of polymorphism
    in Node    *fn_source; // The function as per the source code

    inout Nodes args;
    out size_t  args_count;

    in bool    is_method;
    in bool    is_trait;
    inout bool is_polymorph;

    out bool   is_typed_variadics_direct;
    out size_t typed_variadics_count;

    in Token end;

#undef in
#undef out
#undef inout
} Call_Checker;

void check_call_arity(
    Compiler *c,
    Node     *fn,
    size_t    args_count,
    Token     end,
    bool      is_method,
    size_t    args_count_min,
    size_t    args_count_max,
    Node     *excess_argument);
void check_call_arguments(Compiler *c, Call_Checker *cc, bool check_arguments_provided);

const char *fn_type_to_cstr_but_excluding_receiver_if_required(const Type_Fn *fn_spec_raw, bool exclude_receiver);
void        show_note_about_the_function_being_called(Node *fn, bool is_method, const Type_Fn *fn_spec);

// Methods /////////////////////////////////////////////////////////////////////////////////////////
void check_that_methods_can_be_accessed(Compiler *c, Node *receiver);

bool get_method_spec(
    Compiler    *c,
    Node        *receiver_node,
    Type         receiver_type,
    SV           name,
    Method_Spec *spec,
    Module      *defining_in_module,
    bool        *is_named);

Node_Fn *get_method(Compiler *c, Method_Spec spec, Module *module);
Node_Fn *get_operator_overload(Compiler *c, const char *operator, Node * receiver, Node *op, Module *module);
Node_Fn *get_operator_overload_ex(
    Compiler *c,
    const char *operator,
    Type    receiver,
    Node   *op,
    Module *module,
    bool    monomorphize_if_needed,
    Node   *n,
    i64     group_index);

bool check_signature_of_arithmetic_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec, bool can_be_unary);
void check_signature_of_binary_comparison_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec);
void check_signature_of_index_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec);
void check_signature_of_slice_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec);

// Monomorphizer ///////////////////////////////////////////////////////////////////////////////////
void show_current_monomorphization(Compiler *c);

void add_monomorph_parameter(
    Compiler *c, Node_Polymorph *polymorph, Type type, Const_Value value, Node_Polymorph *to_polymorph);
void add_monomorph_parameter_default_value(
    Compiler       *c,
    Node_Polymorph *polymorph,
    Type            type,
    Const_Value    *default_value,
    Node           *default_value_as_caller_location);
void infer_monomorph_parameters(Compiler *c, const Type *actual, const Type *expected, Node *n, i64 group_index);

Node *monomorphize(Compiler *c, Node *n, Node *site);

// Expressions /////////////////////////////////////////////////////////////////////////////////////
void check_expr_atom(Compiler *c, Node_Atom *atom, Ref_Kind ref, bool *is_ref_valid);
void check_expr_group(Compiler *c, Node_Group *group, Ref_Kind ref, bool *is_ref_valid);
void check_expr_unary(Compiler *c, Node_Unary *unary, bool *is_ref_valid);
void check_expr_binary(Compiler *c, Node_Binary *binary, bool check_children);
void check_expr_member(Compiler *c, Node_Member *member, Ref_Kind ref, bool *is_ref_valid);
void check_expr_enum(Compiler *c, Node_Enum *enumm);
void check_expr_trait(Compiler *c, Node_Trait *trait);
void check_expr_union(Compiler *c, Node_Union *unionn);
void check_expr_struct(Compiler *c, Node_Struct *structt);
void check_expr_compound(Compiler *c, Node_Compound *compound);
void check_expr_call(Compiler *c, Node_Call *call);
void check_expr_index(Compiler *c, Node_Index *index, Ref_Kind ref, bool *is_ref_valid);
void check_expr_indexable(Compiler *c, Node_Indexable *indexable, Ref_Kind ref, bool *is_ref_valid);
void check_expr(Compiler *c, Node *n, Ref_Kind ref);

void check_fn(
    Compiler *c,
    Node_Fn  *fn,
    Ref_Kind  ref,
    bool     *is_ref_valid,
    bool      only_check_polymorphic_parameters,
    bool      only_check_signature);

// Statements //////////////////////////////////////////////////////////////////////////////////////
void        check_switch_expr_and_alloc_preds(Compiler *c, Node_Switch *sw);
Const_Value check_switch_pred(Compiler *c, Node_Switch *sw, Node *pred, size_t *iota);

void check_stmt_assert(Compiler *c, Node_Assert *assertt);
void check_stmt_define(Compiler *c, Node_Define *define);
void check_stmt_block(Compiler *c, Node_Block *block);
void check_stmt_if(Compiler *c, Node_If *iff);
void check_stmt_for(Compiler *c, Node_For *forr);
void check_stmt_switch(Compiler *c, Node_Switch *sw);
void check_stmt_return(Compiler *c, Node_Return *returnn);
void check_stmt(Compiler *c, Node *n);

// Exit Wrapper ////////////////////////////////////////////////////////////////////////////////////
#ifdef DONT_DEFINE_EXIT_WRAPPER
#undef DONT_DEFINE_EXIT_WRAPPER
#else
#define exit(c, code) (show_current_monomorphization(c), exit(code))
#endif // DONT_DEFINE_EXIT_WRAPPER

#endif // CHECKER_INTERNAL_H
