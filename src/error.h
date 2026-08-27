#ifndef ERROR_H
#define ERROR_H

#include "node.h"

typedef enum {
    EK_NOTE,
    EK_WARN,
    EK_ERROR,
    EK_BLANK,
} Error_Kind;

Pos   get_leftmost_pos_of_node(const Node *n);
Token get_leftmost_token_of_node(const Node *n);
Token get_rightmost_token_of_node(const Node *n);

void error_node_begin(Error_Kind kind, const Node *n);
void error_parts_begin(Error_Kind kind, SV sv, Pos pos);
void error_range_begin(Error_Kind kind, Pos begin, Pos end);
void error_token_begin(Error_Kind kind, Token token);
void error_token_range_begin(Error_Kind kind, Token begin, Token end);
void error_standalone_begin(Error_Kind kind);
void error_finalize(void);

void error_node(Error_Kind kind, const Node *n, const char *fmt, ...) Printf_Like(3);
void error_parts(Error_Kind kind, SV sv, Pos pos, const char *fmt, ...) Printf_Like(4);
void error_range(Error_Kind kind, Pos begin, Pos end, const char *fmt, ...) Printf_Like(4);
void error_token(Error_Kind kind, Token token, const char *fmt, ...) Printf_Like(3);
void error_token_range(Error_Kind kind, Token begin, Token end, const char *fmt, ...) Printf_Like(4);
void error_standalone(Error_Kind kind, const char *fmt, ...) Printf_Like(2);

typedef enum {
    WARN_REDUNDANT_STATIC,
    WARN_REDUNDANT_DISTINCT,
    COUNT_WARNING_KINDS
} Warning_Kind;

void warnings_add(Warning_Kind kind, Token token);
void warnings_flush(void);

#endif // ERROR_H
