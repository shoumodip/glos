#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "node.h"

void error_number_of_values_mismatch(
    Node *node, size_t lhs_count, size_t rhs_count, const char *lhs_label, const char *rhs_label);

typedef DA(const char *) Paths;

typedef struct {
    Lexer lexer;
    Token ahead;
    bool  peeked;

    bool in_loop;
    bool in_defer;
    bool in_extern;
    bool in_compile_time_condition;

    bool after_private;

    Node_Fn    *fn_current;
    Polymorphs *polymorphs;
} Parser_State;

typedef struct {
    Paths paths;

    SV cwd;
    SV std;
    SV root;

    Parser_State state;

    Module  *module_current;
    Modules *modules;
} Parser;

Module *module_get(Parser *p, const char *path); // `path` is absolute

typedef enum {
    PARSE_OK,
    PARSE_FAILURE,
    PARSE_EMPTY_DIRECTORY,
} Parse_Result;

void parser_free(Parser *p);

// Return true if actually imported
// Return false if was imported before, and reusing that
bool parser_import(Parser *p, Node_Import *import);

Parse_Result parse_file(Parser *p, const char *path);
Parse_Result parse_directory(Parser *p, const char *path);

#endif // PARSER_H
