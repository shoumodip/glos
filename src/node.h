#ifndef NODE_H
#define NODE_H

#include "token.h"

typedef struct Node Node;

typedef struct {
    Node *head;
    Node *tail;
} Nodes;

typedef struct {
    bool   local;
    size_t iota;
} CompileData;

typedef enum {
    TYPE_UNIT,
    TYPE_BOOL,
    TYPE_I64,

    TYPE_FN,

    COUNT_TYPES
} TypeKind;

typedef struct {
    TypeKind    kind;
    Node       *spec;
    CompileData compile;
} Type;

const char *type_to_cstr(Type type);

bool type_eq(Type a, Type b);
bool type_is_integer(Type type);

typedef enum {
    NODE_ATOM,
    NODE_CALL,
    NODE_UNARY,
    NODE_BINARY,

    NODE_IF,
    NODE_BLOCK,
    NODE_RETURN,

    NODE_FN,
    NODE_VAR,

    NODE_PRINT,
    COUNT_NODES
} NodeKind;

struct Node {
    NodeKind kind;
    Type     type;
    Token    token;
    Node    *next;

    CompileData compile;
};

typedef struct {
    Node  node;
    Node *definition;
} NodeAtom;

typedef struct {
    Node  node;
    Node *fn;

    Nodes  args;
    size_t arity;
} NodeCall;

typedef struct {
    Node  node;
    Node *operand;
} NodeUnary;

typedef struct {
    Node  node;
    Node *lhs;
    Node *rhs;
} NodeBinary;

typedef struct {
    Node  node;
    Node *condition;
    Node *consequence;
    Node *antecedence;
} NodeIf;

typedef struct {
    Node  node;
    Nodes body;
} NodeBlock;

typedef struct {
    Node  node;
    Node *value;
} NodeReturn;

typedef struct {
    Node node;

    Nodes  args;
    size_t arity;

    Node *ret;
    Node *body;
    bool  local;
} NodeFn;

Type node_fn_return_type(const NodeFn *fn);

typedef struct {
    Node  node;
    Node *expr;
    Node *type;
    bool  local;
} NodeVar;

typedef struct {
    Node  node;
    Node *operand;
} NodePrint;

#endif // NODE_H
