#ifndef AST_H
#define AST_H

#include "lexer.h"

typedef struct Node Node;

typedef enum {
    TYPE_BOOL,
    TYPE_I64,
    COUNT_TYPES
} TypeKind;

typedef struct {
    TypeKind kind;
} Type;

bool        typeEq(Type a, Type b);
const char *typeToString(Type type);

typedef enum {
    NODE_ATOM,
    NODE_UNARY,
    NODE_BINARY,

    NODE_PRINT,
    COUNT_NODES
} NodeKind;

typedef struct {
    Node *head;
    Node *tail;
} Nodes;

typedef struct {
    Node *operand;
} NodeUnary;

typedef struct {
    Node *lhs;
    Node *rhs;
} NodeBinary;

typedef struct {
    Node *operand;
} NodePrint;

struct Node {
    NodeKind kind;
    Type     type;
    Token    token;

    union {
        NodeUnary  unary;
        NodeBinary binary;

        NodePrint print;
    } as;

    Node *next;
};

#endif // AST_H
