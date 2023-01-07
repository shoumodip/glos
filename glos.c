#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

// Misc
size_t min(size_t a, size_t b)
{
    return a < b ? a : b;
}

size_t max(size_t a, size_t b)
{
    return a > b ? a : b;
}

// Arena
#define ARENA_CAP 1024
typedef struct {
    char data[ARENA_CAP];
    size_t size;
} Arena;

void arena_push(Arena *arena, char *data, size_t size)
{
    assert(arena->size + size <= ARENA_CAP);
    memcpy(arena->data + arena->size, data, size);
    arena->size += size;
}

void arena_push_char(Arena *arena, char ch)
{
    arena_push(arena, &ch, 1);
}

// Str
typedef struct {
    char *data;
    size_t size;
} Str;

void print_str(FILE *file, Str str)
{
    fwrite(str.data, str.size, 1, file);
}

bool str_eq(Str a, Str b)
{
    return a.size == b.size && memcmp(a.data, b.data, b.size) == 0;
}

bool str_ends_with(Str a, Str b)
{
    return a.size >= b.size && memcmp(a.data + a.size - b.size, b.data, b.size) == 0;
}

bool str_starts_with(Str a, Str b)
{
    return a.size >= b.size && memcmp(a.data, b.data, b.size) == 0;
}

Str str_drop_left(Str str, int n)
{
    n = min(n, str.size);
    str.data += n;
    str.size -= n;
    return str;
}

Str str_trim_left(Str str, char ch)
{
    size_t n = 0;
    while (n < str.size && str.data[n] == ch) {
        n += 1;
    }
    return str_drop_left(str, n);
}

Str str_split_at(Str *str, size_t n)
{
    n = min(n, str->size);
    Str head = *str;
    head.size = n;
    *str = str_drop_left(*str, n + 1);
    return head;
}

Str str_split_by(Str *str, char ch)
{
    size_t i = 0;
    while (i < str->size && str->data[i] != ch) {
        i += 1;
    }
    return str_split_at(str, i);
}

Str str_from_cstr(char *data)
{
    Str str;
    str.data = data;
    str.size = strlen(data);
    return str;
}

bool int_from_str(Str str, size_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < str.size; i += 1) {
        if (isdigit(str.data[i])) {
            n = n * 10 + str.data[i] - '0';
        } else {
            return false;
        }
    }

    *out = n;
    return true;
}

// OS
bool read_file(Str *out, char *path)
{
    int fd = open(path, O_RDONLY, 420);
    if (fd < 0) {
        return false;
    }

    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0) {
        close(fd);
        return false;
    }

    out->size = statbuf.st_size;
    out->data = mmap(NULL, out->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return out->data != MAP_FAILED;
}

int execute_command(char **args, bool silent)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "error: could not fork child\n");
        exit(1);
    }

    if (pid == 0) {
        if (silent) {
            int fd = open("/dev/null", O_WRONLY, 0);
            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
                fprintf(stderr, "error: could not silence child\n");
                exit(1);
            }
        }

        if (execvp(*args, args) < 0) {
            fprintf(stderr, "error: could not execute child\n");
            exit(1);
        }
    }

    int wstatus;
    if (wait(&wstatus) < 0) {
        fprintf(stderr, "error: could not wait for child\n");
        exit(1);
    }
    return WEXITSTATUS(wstatus);
}

#define MAP_ANONYMOUS 32

int capture_command(char **args, Str *out, Str *err)
{
    int pipe_stdout[2];
    int pipe_stderr[2];

    if (pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0) {
        fprintf(stderr, "error: could not create pipe\n");
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "error: could not fork child\n");
        exit(1);
    }

    if (pid == 0) {
        if (dup2(pipe_stdout[1], STDOUT_FILENO) < 0 || dup2(pipe_stderr[1], STDERR_FILENO) < 0) {
            fprintf(stderr, "error: could not create pipe\n");
            exit(1);
        }

        close(pipe_stdout[1]);
        close(pipe_stderr[1]);

        close(pipe_stdout[0]);
        close(pipe_stderr[0]);

        if (execvp(*args, args) < 0) {
            fprintf(stderr, "error: could not execute child\n");
            exit(1);
        }
    }

    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    int wstatus;
    if (wait(&wstatus) < 0) {
        fprintf(stderr, "error: could not wait for child\n");
        exit(1);
    }
    int code = WEXITSTATUS(wstatus);

    if (ioctl(pipe_stdout[0], FIONREAD, &out->size) < 0 || ioctl(pipe_stderr[0], FIONREAD, &err->size) < 0) {
        fprintf(stderr, "error: could not capture query pipe size\n");
        exit(1);
    }

    if (out->size != 0) {
        out->data = mmap(NULL, out->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        if (out->data == MAP_FAILED || read(pipe_stdout[0], out->data, out->size) < 0) {
            fprintf(stderr, "error: could not capture child stdout\n");
            exit(1);
        }
    }

    if (err->size != 0) {
        err->data = mmap(NULL, err->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        if (err->data == MAP_FAILED || read(pipe_stderr[0], err->data, err->size) < 0) {
            fprintf(stderr, "error: could not capture child stderr\n");
            exit(1);
        }
    }

    close(pipe_stdout[0]);
    close(pipe_stderr[0]);
    return code;
}

// Pos
typedef struct {
    char *path;
    size_t row;
    size_t col;
} Pos;

void print_pos(FILE *file, Pos pos)
{
    fprintf(file, "%s:%zu:%zu: ", pos.path, pos.row, pos.col);
}

// Token
enum {
    TOKEN_EOF,
    TOKEN_INT,
    TOKEN_STR,
    TOKEN_BOOL,
    TOKEN_CHAR,
    TOKEN_CSTR,
    TOKEN_IDENT,

    TOKEN_DOT,
    TOKEN_ARROW,
    TOKEN_COMMA,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,

    TOKEN_BOR,
    TOKEN_BAND,
    TOKEN_BNOT,

    TOKEN_SET,
    TOKEN_ADD_SET,
    TOKEN_SUB_SET,
    TOKEN_MUL_SET,
    TOKEN_DIV_SET,
    TOKEN_MOD_SET,

    TOKEN_LNOT,

    TOKEN_GT,
    TOKEN_GE,
    TOKEN_LT,
    TOKEN_LE,
    TOKEN_EQ,
    TOKEN_NE,

    TOKEN_AS,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_MATCH,

    TOKEN_FN,
    TOKEN_LET,
    TOKEN_CONST,
    TOKEN_STRUCT,

    TOKEN_USE,
    TOKEN_ARGC,
    TOKEN_ARGV,
    TOKEN_ASSERT,
    TOKEN_RETURN,

    TOKEN_PRINT,
    COUNT_TOKENS
};

static_assert(COUNT_TOKENS == 52);
char *cstr_from_token_kind(int kind)
{
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_INT:
        return "integer";

    case TOKEN_STR:
        return "string";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_CHAR:
        return "character";

    case TOKEN_CSTR:
        return "C-string";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_DOT:
        return "'.'";

    case TOKEN_ARROW:
        return "'=>'";

    case TOKEN_COMMA:
        return "','";

    case TOKEN_LPAREN:
        return "'('";

    case TOKEN_RPAREN:
        return "')'";

    case TOKEN_LBRACE:
        return "'{'";

    case TOKEN_RBRACE:
        return "'}'";

    case TOKEN_LBRACKET:
        return "'['";

    case TOKEN_RBRACKET:
        return "']'";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_MOD:
        return "'%'";

    case TOKEN_BOR:
        return "'|'";

    case TOKEN_BAND:
        return "'&'";

    case TOKEN_BNOT:
        return "'~'";

    case TOKEN_SET:
        return "'='";

    case TOKEN_ADD_SET:
        return "'+='";

    case TOKEN_SUB_SET:
        return "'-='";

    case TOKEN_MUL_SET:
        return "'*='";

    case TOKEN_DIV_SET:
        return "'/='";

    case TOKEN_MOD_SET:
        return "'%='";

    case TOKEN_LNOT:
        return "'!'";

    case TOKEN_GT:
        return "'>'";

    case TOKEN_GE:
        return "'>='";

    case TOKEN_LT:
        return "'<'";

    case TOKEN_LE:
        return "'<='";

    case TOKEN_EQ:
        return "'=='";

    case TOKEN_NE:
        return "'!='";

    case TOKEN_AS:
        return "'as'";

    case TOKEN_IF:
        return "keyword 'if'";

    case TOKEN_ELSE:
        return "keyword 'else'";

    case TOKEN_FOR:
        return "keyword 'for'";

    case TOKEN_MATCH:
        return "keyword 'match'";

    case TOKEN_FN:
        return "keyword 'fn'";

    case TOKEN_LET:
        return "keyword 'let'";

    case TOKEN_CONST:
        return "keyword 'const'";

    case TOKEN_STRUCT:
        return "keyword 'struct'";

    case TOKEN_USE:
        return "keyword 'use'";

    case TOKEN_ARGC:
        return "keyword 'argc'";

    case TOKEN_ARGV:
        return "keyword 'argv'";

    case TOKEN_ASSERT:
        return "keyword 'assert'";

    case TOKEN_RETURN:
        return "keyword 'return'";

    case TOKEN_PRINT:
        return "keyword 'print'";

    default: assert(0 && "unreachable");
    }
}

typedef struct {
    int kind;
    size_t data;

    Pos pos;
    Str str;
} Token;

// Lexer
typedef struct {
    Pos pos;
    Str str;
    bool peeked;
    Token buffer;
    size_t prev_row;
} Lexer;

Lexer lexer;

void lexer_open(char *path)
{
    if (!read_file(&lexer.str, path)) {
        if (lexer.peeked) {
            print_pos(stderr, lexer.buffer.pos);
        }

        fprintf(stderr, "error: could not read file '%s'\n", path);
        exit(1);
    }

    lexer.pos.path = path;
    lexer.pos.row = 1;
    lexer.pos.col = 1;
    lexer.peeked = false;
}

void lexer_buffer(Token token)
{
    lexer.peeked = true;
    lexer.buffer = token;
}

void lexer_advance(void)
{
    if (*lexer.str.data == '\n') {
        lexer.pos.row += 1;
        lexer.pos.col = 1;
    } else {
        lexer.pos.col += 1;
    }

    lexer.str.data += 1;
    lexer.str.size -= 1;
}

char lexer_consume(void)
{
    lexer_advance();
    return lexer.str.data[-1];
}

bool lexer_match(char ch)
{
    if (lexer.str.size > 0 && *lexer.str.data == ch) {
        lexer_advance();
        return true;
    }
    return false;
}

void error_invalid(char *name)
{
    print_pos(stderr, lexer.pos);
    fprintf(stderr, "error: invalid %s '%c'\n", name, lexer.str.data[-1]);
    exit(1);
}

void error_unterminated(char *name)
{
    print_pos(stderr, lexer.pos);
    fprintf(stderr, "error: unterminated %s\n", name);
    exit(1);
}

char lexer_char(char *name)
{
    if (lexer.str.size == 0) {
        error_unterminated(name);
    }

    char ch = lexer_consume();
    if (ch == '\\') {
        if (lexer.str.size == 0) {
            error_unterminated("escape character");
        }

        Pos pos = lexer.pos;
        switch (lexer_consume()) {
        case 'n':
            ch = '\n';
            break;

        case 't':
            ch = '\t';
            break;

        case '0':
            ch = '\0';
            break;

        case '"':
            ch = '"';
            break;

        case '\'':
            ch = '\'';
            break;

        case '\\':
            ch = '\\';
            break;

        default:
            lexer.pos = pos;
            error_invalid("escape character");
        }
    }

    return ch;
}

static_assert(COUNT_TOKENS == 52);
Token lexer_next(void)
{
    if (lexer.peeked) {
        lexer.peeked = false;
        lexer.prev_row = lexer.buffer.pos.row;
        return lexer.buffer;
    }

    while (lexer.str.size > 0) {
        if (isspace(*lexer.str.data)) {
            lexer_advance();
        } else if (lexer_match('#')) {
            if (lexer_match('#')) {
                while (lexer.str.size > 0) {
                    if (lexer_match('#') && lexer_match('#')) {
                        break;
                    }

                    lexer_advance();
                }
            } else {
                while (lexer.str.size > 0 && *lexer.str.data != '\n') {
                    lexer_advance();
                }
            }
        } else {
            break;
        }
    }

    Token token;
    token.pos = lexer.pos;
    token.str = lexer.str;

    if (lexer.str.size == 0) {
        token.kind = TOKEN_EOF;
    } else if (isdigit(*lexer.str.data)) {
        token.kind = TOKEN_INT;
        token.data = 0;

        while (lexer.str.size > 0 && isdigit(*lexer.str.data)) {
            token.data = token.data * 10 + lexer_consume() - '0';
        }

        token.str.size -= lexer.str.size;
    } else if (isalpha(*lexer.str.data) || *lexer.str.data == '_') {
        while (lexer.str.size > 0 && (isalnum(*lexer.str.data) || *lexer.str.data == '_')) {
            lexer_advance();
        }

        token.str.size -= lexer.str.size;

        if (str_eq(token.str, str_from_cstr("true"))) {
            token.kind = TOKEN_BOOL;
            token.data = 1;
        } else if (str_eq(token.str, str_from_cstr("false"))) {
            token.kind = TOKEN_BOOL;
            token.data = 0;
        } else if (str_eq(token.str, str_from_cstr("as"))) {
            token.kind = TOKEN_AS;
        } else if (str_eq(token.str, str_from_cstr("if"))) {
            token.kind = TOKEN_IF;
        } else if (str_eq(token.str, str_from_cstr("else"))) {
            token.kind = TOKEN_ELSE;
        } else if (str_eq(token.str, str_from_cstr("for"))) {
            token.kind = TOKEN_FOR;
        } else if (str_eq(token.str, str_from_cstr("match"))) {
            token.kind = TOKEN_MATCH;
        } else if (str_eq(token.str, str_from_cstr("fn"))) {
            token.kind = TOKEN_FN;
        } else if (str_eq(token.str, str_from_cstr("let"))) {
            token.kind = TOKEN_LET;
        } else if (str_eq(token.str, str_from_cstr("const"))) {
            token.kind = TOKEN_CONST;
        } else if (str_eq(token.str, str_from_cstr("struct"))) {
            token.kind = TOKEN_STRUCT;
        } else if (str_eq(token.str, str_from_cstr("use"))) {
            token.kind = TOKEN_USE;
        } else if (str_eq(token.str, str_from_cstr("argc"))) {
            token.kind = TOKEN_ARGC;
        } else if (str_eq(token.str, str_from_cstr("argv"))) {
            token.kind = TOKEN_ARGV;
        } else if (str_eq(token.str, str_from_cstr("assert"))) {
            token.kind = TOKEN_ASSERT;
        } else if (str_eq(token.str, str_from_cstr("return"))) {
            token.kind = TOKEN_RETURN;
        } else if (str_eq(token.str, str_from_cstr("print"))) {
            token.kind = TOKEN_PRINT;
        } else {
            token.kind = TOKEN_IDENT;
        }
    } else {
        switch (lexer_consume()) {
        case '\'':
            token.kind = TOKEN_CHAR;
            token.data = lexer_char("character");
            if (!lexer_match('\'')) {
                error_unterminated("character");
            }
            break;

        case '"':
            while (true) {
                char ch = lexer_char("string");
                if (ch == '"' && lexer.str.data[-2] != '\\') {
                    break;
                }
            }

            if (lexer_match('c')) {
                token.kind = TOKEN_CSTR;
            } else {
                token.kind = TOKEN_STR;
            }
            break;

        case '.':
            token.kind = TOKEN_DOT;
            break;

        case ',':
            token.kind = TOKEN_COMMA;
            break;

        case '(':
            token.kind = TOKEN_LPAREN;
            break;

        case ')':
            token.kind = TOKEN_RPAREN;
            break;

        case '{':
            token.kind = TOKEN_LBRACE;
            break;

        case '}':
            token.kind = TOKEN_RBRACE;
            break;

        case '[':
            token.kind = TOKEN_LBRACKET;
            break;

        case ']':
            token.kind = TOKEN_RBRACKET;
            break;

        case '+':
            if (lexer_match('=')) {
                token.kind = TOKEN_ADD_SET;
            } else {
                token.kind = TOKEN_ADD;
            }
            break;

        case '-':
            if (lexer_match('=')) {
                token.kind = TOKEN_SUB_SET;
            } else {
                token.kind = TOKEN_SUB;
            }
            break;

        case '*':
            if (lexer_match('=')) {
                token.kind = TOKEN_MUL_SET;
            } else {
                token.kind = TOKEN_MUL;
            }
            break;

        case '/':
            if (lexer_match('=')) {
                token.kind = TOKEN_DIV_SET;
            } else {
                token.kind = TOKEN_DIV;
            }
            break;

        case '%':
            if (lexer_match('=')) {
                token.kind = TOKEN_MOD_SET;
            } else {
                token.kind = TOKEN_MOD;
            }
            break;

        case '|':
            token.kind = TOKEN_BOR;
            break;

        case '&':
            token.kind = TOKEN_BAND;
            break;

        case '~':
            token.kind = TOKEN_BNOT;
            break;

        case '>':
            if (lexer_match('=')) {
                token.kind = TOKEN_GE;
            } else {
                token.kind = TOKEN_GT;
            }
            break;

        case '<':
            if (lexer_match('=')) {
                token.kind = TOKEN_LE;
            } else {
                token.kind = TOKEN_LT;
            }
            break;

        case '=':
            if (lexer_match('>')) {
                token.kind = TOKEN_ARROW;
            } else if (lexer_match('=')) {
                token.kind = TOKEN_EQ;
            } else {
                token.kind = TOKEN_SET;
            }
            break;

        case '!':
            if (lexer_match('=')) {
                token.kind = TOKEN_NE;
            } else {
                token.kind = TOKEN_LNOT;
            }
            break;

        default:
            lexer.pos = token.pos;
            error_invalid("character");
        }

        token.str.size -= lexer.str.size;
    }

    lexer.prev_row = token.pos.row;
    return token;
}

Token lexer_peek(void)
{
    int prev_row = lexer.prev_row;
    if (!lexer.peeked) {
        lexer_buffer(lexer_next());
        lexer.prev_row = prev_row;
    }
    return lexer.buffer;
}

bool lexer_read(int kind)
{
    lexer_peek();
    lexer.peeked = lexer.buffer.kind != kind;
    return !lexer.peeked;
}

Token lexer_expect(int kind)
{
    Token token = lexer_next();
    if (token.kind != kind) {
        print_pos(stderr, token.pos);
        fprintf(stderr, "error: expected %s, got %s\n",
                cstr_from_token_kind(kind),
                cstr_from_token_kind(token.kind));
        exit(1);
    }
    return token;
}

Token lexer_either(int a, int b)
{
    Token token = lexer_next();
    if (token.kind != a && token.kind != b) {
        print_pos(stderr, token.pos);
        fprintf(stderr, "error: expected %s or %s, got %s\n",
                cstr_from_token_kind(a),
                cstr_from_token_kind(b),
                cstr_from_token_kind(token.kind));
        exit(1);
    }
    return token;
}

bool lexer_peek_row(Token *token)
{
    *token = lexer_peek();
    return token->pos.row == lexer.prev_row;
}

// AST
enum {
    TYPE_NIL,
    TYPE_INT,
    TYPE_BOOL,
    TYPE_CHAR,
    TYPE_ARRAY,
    TYPE_STRUCT,
    COUNT_TYPES
};

typedef struct {
    int kind;
    size_t ref;
    size_t data;
} Type;

enum {
    NODE_ATOM,
    NODE_CALL,
    NODE_UNARY,
    NODE_BINARY,

    NODE_BLOCK,
    NODE_IF,
    NODE_FOR,
    NODE_MATCH,
    NODE_BRANCH,

    NODE_FN,
    NODE_LET,
    NODE_CONST,
    NODE_STRUCT,

    NODE_ASSERT,
    NODE_RETURN,

    NODE_PRINT,
    COUNT_NODES
};

#define NODE_CALL_ARGS 0

#define NODE_CAST_TYPE 0
#define NODE_CAST_EXPR 1

#define NODE_UNARY_EXPR 0

#define NODE_BINARY_LHS 0
#define NODE_BINARY_RHS 1

#define NODE_BLOCK_START 0

#define NODE_IF_COND 0
#define NODE_IF_THEN 1
#define NODE_IF_ELSE 2

#define NODE_FOR_INIT 0
#define NODE_FOR_COND 1
#define NODE_FOR_UPDATE 2
#define NODE_FOR_BODY 3

#define NODE_MATCH_EXPR 0
#define NODE_MATCH_LIST 1
#define NODE_MATCH_ELSE 2

#define NODE_BRANCH_LIST 0
#define NODE_BRANCH_BODY 1

#define NODE_FN_ARGS 0
#define NODE_FN_TYPE 1
#define NODE_FN_BODY 2

#define NODE_LET_EXPR 0
#define NODE_LET_TYPE 1

#define NODE_CONST_EXPR 0
#define NODE_CONST_LIST 1

#define NODE_STRUCT_FIELDS 0

#define NODE_ASSERT_EXPR 0

#define NODE_RETURN_EXPR 0

#define NODE_PRINT_EXPR 0

typedef struct {
    int kind;
    Type type;
    Token token;

    size_t nodes[4];
    size_t next;
} Node;

#define NODES_CAP 1024
Node nodes[NODES_CAP];
size_t nodes_count;

size_t node_new(int kind, Token token)
{
    assert(nodes_count < NODES_CAP);
    nodes[nodes_count].kind = kind;
    nodes[nodes_count].token = token;
    nodes_count += 1;
    return nodes_count - 1;
}

size_t *node_list_push(size_t *list, size_t node)
{
    if (*list != 0) {
        list = &nodes[*list].next;
    }

    *list = node;
    return list;
}

bool node_list_find(size_t list, size_t node)
{
    while (list != 0) {
        if (str_eq(nodes[list].token.str, nodes[node].token.str)) {
            nodes[node].token.data = list;
            return true;
        }
        list = nodes[list].next;
    }
    return false;
}

// Parser
enum {
    POWER_NIL,
    POWER_SET,
    POWER_CMP,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_AS,
    POWER_PRE,
    POWER_DOT,
    POWER_IDX,
};

static_assert(COUNT_TOKENS == 52);
int power_from_token_kind(int kind)
{
    switch (kind) {
    case TOKEN_DOT:
        return POWER_DOT;

    case TOKEN_LBRACKET:
        return POWER_IDX;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return POWER_MUL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_SET:
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
        return POWER_SET;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return POWER_CMP;

    case TOKEN_AS:
        return POWER_AS;

    default: return POWER_NIL;
    }
}

void error_unexpected(Token token)
{
    print_pos(stderr, token.pos);
    fprintf(stderr, "error: unexpected %s\n", cstr_from_token_kind(token.kind));
    exit(1);
}

static_assert(COUNT_TOKENS == 52);
size_t parse_const(int mbp)
{
    size_t node;
    Token token = lexer_next();

    switch (token.kind) {
    case TOKEN_LPAREN:
        node = parse_const(POWER_SET);
        lexer_expect(TOKEN_RPAREN);
        break;

    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
    case TOKEN_IDENT:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_BNOT:
    case TOKEN_LNOT:
        node = node_new(NODE_UNARY, token);
        nodes[node].nodes[NODE_UNARY_EXPR] = parse_const(POWER_PRE);
        break;

    default: error_unexpected(token);
    }

    while (true) {
        if (!lexer_peek_row(&token)) {
            break;
        }

        int lbp = power_from_token_kind(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexer.peeked = false;

        size_t binary = node_new(NODE_BINARY, token);
        nodes[binary].nodes[NODE_BINARY_LHS] = node;
        switch (token.kind) {
        case TOKEN_AS:
        case TOKEN_DOT:
        case TOKEN_LBRACKET:
            error_unexpected(token);
            break;

        default:
            nodes[binary].nodes[NODE_BINARY_RHS] = parse_const(lbp);
        }
        node = binary;
    }

    return node;
}

size_t parse_type(void)
{
    size_t node;
    Token token = lexer_next();
    switch (token.kind) {
    case TOKEN_IDENT:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_MUL:
        node = node_new(NODE_UNARY, token);
        nodes[node].nodes[NODE_UNARY_EXPR] = parse_type();
        break;

    case TOKEN_LBRACKET:
        node = node_new(NODE_BINARY, token);
        nodes[node].nodes[NODE_BINARY_LHS] = parse_const(POWER_SET);
        lexer_expect(TOKEN_RBRACKET);
        nodes[node].nodes[NODE_BINARY_RHS] = parse_type();
        break;

    default: error_unexpected(token);
    }

    return node;
}

static_assert(COUNT_TOKENS == 52);
size_t parse_expr(int mbp)
{
    size_t node;
    Token token = lexer_next();

    switch (token.kind) {
    case TOKEN_LPAREN:
        node = parse_expr(POWER_SET);
        lexer_expect(TOKEN_RPAREN);
        break;

    case TOKEN_INT:
    case TOKEN_STR:
    case TOKEN_ARGC:
    case TOKEN_ARGV:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
    case TOKEN_CSTR:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_IDENT:
        if (lexer_peek_row(&lexer.buffer) && lexer.buffer.kind == TOKEN_LPAREN) {
            lexer.peeked = false;
            node = node_new(NODE_CALL, token);

            nodes[node].token.data = 0;
            if (!lexer_read(TOKEN_RPAREN)) {
                size_t *args = &nodes[node].nodes[NODE_CALL_ARGS];
                while (true) {
                    args = node_list_push(args, parse_expr(POWER_SET));
                    token = lexer_either(TOKEN_COMMA, TOKEN_RPAREN);
                    nodes[node].token.data += 1;
                    if (token.kind == TOKEN_RPAREN) {
                        break;
                    }
                }
            }
        } else {
            node = node_new(NODE_ATOM, token);
        }
        break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BNOT:
    case TOKEN_BAND:
    case TOKEN_LNOT:
        node = node_new(NODE_UNARY, token);
        nodes[node].nodes[NODE_UNARY_EXPR] = parse_expr(POWER_PRE);
        break;

    default: error_unexpected(token);
    }

    while (true) {
        if (!lexer_peek_row(&token)) {
            break;
        }

        int lbp = power_from_token_kind(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexer.peeked = false;

        size_t binary = node_new(NODE_BINARY, token);
        nodes[binary].nodes[NODE_BINARY_LHS] = node;
        switch (token.kind) {
        case TOKEN_DOT:
            nodes[binary].nodes[NODE_BINARY_RHS] = node_new(NODE_ATOM, lexer_expect(TOKEN_IDENT));
            break;

        case TOKEN_LBRACKET:
            nodes[binary].nodes[NODE_BINARY_RHS] = parse_expr(POWER_SET);
            lexer_expect(TOKEN_RBRACKET);
            break;

        case TOKEN_AS:
            nodes[binary].nodes[NODE_BINARY_RHS] = parse_type();
            break;

        default:
            nodes[binary].nodes[NODE_BINARY_RHS] = parse_expr(lbp);
        }
        node = binary;
    }

    return node;
}

bool parser_local;

void local_assert(Token token, bool expected)
{
    if (parser_local != expected) {
        print_pos(stderr, token.pos);
        fprintf(stderr, "error: unexpected %s in %s scope\n",
                cstr_from_token_kind(token.kind),
                parser_local ? "local" : "global");
        exit(1);
    }
}

size_t parse_decl(void)
{
    size_t node = node_new(NODE_LET, lexer_expect(TOKEN_IDENT));
    nodes[node].token.data = parser_local;
    nodes[node].nodes[NODE_LET_TYPE] = parse_type();
    return node;
}

size_t tops_base;
size_t *tops_iter;
Arena path_arena;

#define IMPORTS_CAP 1024
Str imports[IMPORTS_CAP];
size_t imports_count;

void imports_push(Str path)
{
    assert(imports_count < IMPORTS_CAP);
    imports[imports_count] = path;
    imports_count += 1;
}

bool imports_find(Str path)
{
    for (size_t i = imports_count; i > 0; i -= 1) {
        if (str_eq(imports[i - 1], path)) {
            return true;
        }
    }
    return false;
}

static_assert(COUNT_TOKENS == 52);
size_t parse_stmt(void)
{
    size_t node;
    Token token = lexer_next();

    switch (token.kind) {
    case TOKEN_LBRACE:
        local_assert(token, true);
        node = node_new(NODE_BLOCK, token);
        for (size_t *list = &nodes[node].nodes[NODE_BLOCK_START]; !lexer_read(TOKEN_RBRACE); ) {
            list = node_list_push(list, parse_stmt());
        }
        break;

    case TOKEN_IF:
        local_assert(token, true);
        node = node_new(NODE_IF, token);
        nodes[node].nodes[NODE_IF_COND] = parse_expr(POWER_SET);

        lexer_buffer(lexer_expect(TOKEN_LBRACE));
        nodes[node].nodes[NODE_IF_THEN] = parse_stmt();

        if (lexer_read(TOKEN_ELSE)) {
            lexer_buffer(lexer_expect(TOKEN_LBRACE));
            nodes[node].nodes[NODE_IF_ELSE] = parse_stmt();
        }
        break;

    case TOKEN_FOR:
        local_assert(token, true);
        node = node_new(NODE_FOR, token);

        token = lexer_peek();
        if (token.kind == TOKEN_LET) {
            nodes[node].nodes[NODE_FOR_INIT] = parse_stmt();
            lexer_expect(TOKEN_COMMA);
            nodes[node].nodes[NODE_FOR_COND] = parse_expr(POWER_SET);

            if (lexer_read(TOKEN_COMMA)) {
                nodes[node].nodes[NODE_FOR_UPDATE] = parse_expr(POWER_NIL);
            }
        } else {
            nodes[node].nodes[NODE_FOR_COND] = parse_expr(POWER_SET);
        }

        lexer_buffer(lexer_expect(TOKEN_LBRACE));
        nodes[node].nodes[NODE_FOR_BODY] = parse_stmt();
        break;

    case TOKEN_MATCH: {
        local_assert(token, true);
        node = node_new(NODE_MATCH, token);

        nodes[node].nodes[NODE_MATCH_EXPR] = parse_expr(POWER_SET);
        size_t *branches = &nodes[node].nodes[NODE_MATCH_LIST];

        lexer_expect(TOKEN_LBRACE);
        while (!lexer_read(TOKEN_RBRACE)) {
            if (lexer_read(TOKEN_ELSE)) {
                lexer_expect(TOKEN_ARROW);
                nodes[node].nodes[NODE_MATCH_ELSE] = parse_stmt();
                lexer_expect(TOKEN_RBRACE);
                break;
            } else {
                size_t branch = node_new(NODE_BRANCH, token);
                size_t *preds = &nodes[branch].nodes[NODE_BRANCH_LIST];
                while (true) {
                    preds = node_list_push(preds, parse_const(POWER_SET));
                    token = lexer_either(TOKEN_COMMA, TOKEN_ARROW);
                    if (token.kind == TOKEN_ARROW) {
                        break;
                    }
                }

                nodes[branch].nodes[NODE_BRANCH_BODY] = parse_stmt();
                branches = node_list_push(branches, branch);
            }
        }
    } break;

    case TOKEN_FN:
        local_assert(token, false);
        node = node_new(NODE_FN, lexer_expect(TOKEN_IDENT));
        lexer_expect(TOKEN_LPAREN);
        parser_local = true;

        if (!lexer_read(TOKEN_RPAREN)) {
            size_t *args = &nodes[node].nodes[NODE_FN_ARGS];
            while (true) {
                args = node_list_push(args, parse_decl());
                token = lexer_either(TOKEN_COMMA, TOKEN_RPAREN);
                if (token.kind == TOKEN_RPAREN) {
                    break;
                }
            }
        }

        token = lexer_peek();
        if (token.kind != TOKEN_LBRACE) {
            nodes[node].nodes[NODE_FN_TYPE] = parse_type();
        }

        lexer_expect(TOKEN_LBRACE);
        nodes[node].nodes[NODE_FN_BODY] = node_new(NODE_BLOCK, token);
        for (size_t *list = &nodes[nodes[node].nodes[NODE_FN_BODY]].nodes[NODE_BLOCK_START]; !lexer_read(TOKEN_RBRACE); ) {
            token = lexer_peek();
            list = node_list_push(list, parse_stmt());
        }

        if (nodes[node].nodes[NODE_FN_TYPE] != 0 && token.kind != TOKEN_RETURN) {
            print_pos(stderr, lexer.buffer.pos);
            fprintf(stderr, "error: expected keyword 'return' before '}'\n");
            exit(1);
        }

        parser_local = false;
        break;

    case TOKEN_LET:
        node = node_new(NODE_LET, lexer_expect(TOKEN_IDENT));
        if (lexer_read(TOKEN_SET)) {
            nodes[node].nodes[NODE_LET_EXPR] = parse_expr(POWER_SET);
        } else {
            nodes[node].nodes[NODE_LET_TYPE] = parse_type();
        }
        nodes[node].token.data = parser_local;
        break;

    case TOKEN_CONST:
        token = lexer_either(TOKEN_IDENT, TOKEN_LPAREN);
        node = node_new(NODE_CONST, token);
        if (token.kind == TOKEN_IDENT) {
            lexer_expect(TOKEN_SET);
            nodes[node].nodes[NODE_CONST_EXPR] = parse_const(POWER_SET);
        } else {
            size_t *iter = &nodes[node].nodes[NODE_CONST_LIST];
            while (!lexer_read(TOKEN_RPAREN)) {
                iter = node_list_push(iter, node_new(NODE_CONST, lexer_expect(TOKEN_IDENT)));
            }
        }
        break;

    case TOKEN_STRUCT: {
        local_assert(token, false);
        node = node_new(NODE_STRUCT, lexer_expect(TOKEN_IDENT));
        lexer_expect(TOKEN_LBRACE);

        size_t *fields = &nodes[node].nodes[NODE_STRUCT_FIELDS];
        while (!lexer_read(TOKEN_RBRACE)) {
            fields = node_list_push(fields, parse_decl());
        }
    } break;

    case TOKEN_USE: {
        local_assert(token, false);

        Str path;
        path.data = path_arena.data + path_arena.size;
        while (true) {
            Token step = lexer_expect(TOKEN_IDENT);
            arena_push(&path_arena, step.str.data, step.str.size);

            if (lexer_read(TOKEN_DOT)) {
                arena_push(&path_arena, "/", 1);
            } else {
                break;
            }
        }
        arena_push(&path_arena, ".glos", 6);
        path.size = path_arena.data + path_arena.size - path.data;

        if (imports_find(path)) {
            path_arena.size -= path.size;
            return 0;
        }
        imports_push(path);

        Lexer lexer_save = lexer;

        lexer_buffer(token);
        lexer_open(path.data);
        for (tops_iter = &tops_base; !lexer_read(TOKEN_EOF); ) {
            size_t stmt = parse_stmt();
            if (stmt != 0) {
                tops_iter = node_list_push(tops_iter, stmt);
            }
        }

        lexer = lexer_save;
        return 0;
    };

    case TOKEN_ASSERT:
        token.data = parser_local;
        node = node_new(NODE_ASSERT, token);
        lexer_expect(TOKEN_LPAREN);
        if (parser_local) {
            nodes[node].nodes[NODE_ASSERT_EXPR] = parse_expr(POWER_SET);
        } else {
            nodes[node].nodes[NODE_ASSERT_EXPR] = parse_const(POWER_SET);
        }
        lexer_expect(TOKEN_RPAREN);
        break;

    case TOKEN_RETURN:
        local_assert(token, true);
        node = node_new(NODE_RETURN, token);
        if (lexer_peek_row(&token)) {
            nodes[node].nodes[NODE_RETURN_EXPR] = parse_expr(POWER_SET);
        }
        break;

    case TOKEN_PRINT:
        local_assert(token, true);
        node = node_new(NODE_PRINT, token);
        nodes[node].nodes[NODE_PRINT_EXPR] = parse_expr(POWER_SET);
        break;

    default:
        local_assert(token, true);
        lexer_buffer(token);
        node = parse_expr(POWER_NIL);
    }

    return node;
}

// Constant Evaluator
static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void eval_const_unary(size_t node)
{
    size_t expr = nodes[node].nodes[NODE_UNARY_EXPR];

    switch (nodes[node].token.kind) {
    case TOKEN_SUB:
        nodes[node].token.data = -nodes[expr].token.data;
        break;

    case TOKEN_BNOT:
        nodes[node].token.data = ~nodes[expr].token.data;
        break;

    case TOKEN_LNOT:
        nodes[node].token.data = !nodes[expr].token.data;
        break;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void eval_const_binary(size_t node)
{
    size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
    size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

    switch (nodes[node].token.kind) {
    case TOKEN_ADD:
        nodes[node].token.data = nodes[lhs].token.data + nodes[rhs].token.data;
        break;

    case TOKEN_SUB:
        nodes[node].token.data = nodes[lhs].token.data - nodes[rhs].token.data;
        break;

    case TOKEN_MUL:
        nodes[node].token.data = nodes[lhs].token.data * nodes[rhs].token.data;
        break;

    case TOKEN_DIV:
        nodes[node].token.data = nodes[lhs].token.data / nodes[rhs].token.data;
        break;

    case TOKEN_MOD:
        nodes[node].token.data = nodes[lhs].token.data % nodes[rhs].token.data;
        break;

    case TOKEN_BOR:
        nodes[node].token.data = nodes[lhs].token.data | nodes[rhs].token.data;
        break;

    case TOKEN_BAND:
        nodes[node].token.data = nodes[lhs].token.data & nodes[rhs].token.data;
        break;

    case TOKEN_GT:
        nodes[node].token.data = nodes[lhs].token.data > nodes[rhs].token.data;
        break;

    case TOKEN_GE:
        nodes[node].token.data = nodes[lhs].token.data >= nodes[rhs].token.data;
        break;

    case TOKEN_LT:
        nodes[node].token.data = nodes[lhs].token.data < nodes[rhs].token.data;
        break;

    case TOKEN_LE:
        nodes[node].token.data = nodes[lhs].token.data <= nodes[rhs].token.data;
        break;

    case TOKEN_EQ:
        nodes[node].token.data = nodes[lhs].token.data == nodes[rhs].token.data;
        break;

    case TOKEN_NE:
        nodes[node].token.data = nodes[lhs].token.data != nodes[rhs].token.data;
        break;

    default: assert(0 && "unreachable");
    }
}

// Type
bool type_eq(Type a, Type b)
{
    return a.kind == b.kind && a.ref == b.ref;
}

bool type_isarray(Type type)
{
    return type.kind == TYPE_ARRAY && type.ref == 0;
}

Type type_new(int kind, size_t ref, size_t data)
{
    Type type;
    type.kind = kind;
    type.ref = ref;
    type.data = data;
    return type;
}

Type type_ref(Type type)
{
    type.ref += 1;
    return type;
}

Type type_deref(Type type)
{
    if (type_isarray(type)) {
        return nodes[nodes[type.data].nodes[NODE_BINARY_RHS]].type;
    }

    assert(type.ref > 0);
    type.ref -= 1;
    return type;
}

static_assert(COUNT_TYPES == 6);
void print_type(FILE *file, Type type)
{
    for (size_t i = 0; i < type.ref; i += 1) {
        fprintf(file, "*");
    }

    switch (type.kind) {
    case TYPE_NIL:
        fprintf(file, "nil");
        break;

    case TYPE_INT:
        fprintf(file, "int");
        break;

    case TYPE_BOOL:
        fprintf(file, "bool");
        break;

    case TYPE_CHAR:
        fprintf(file, "char");
        break;

    case TYPE_ARRAY:
        fprintf(file, "[%zu]", nodes[nodes[type.data].nodes[NODE_BINARY_LHS]].token.data);
        print_type(file, nodes[nodes[type.data].nodes[NODE_BINARY_RHS]].type);
        break;

    case TYPE_STRUCT:
        print_str(file, nodes[type.data].token.str);
        break;

    default: assert(0 && "unreachable");
    }
}

// Scope
#define SCOPE_CAP 1024

size_t constants[SCOPE_CAP];
size_t constants_count;

void constants_push(size_t node)
{
    assert(constants_count < SCOPE_CAP);
    constants[constants_count] = node;
    constants_count += 1;
}

bool constants_find(Str name, size_t *index)
{
    for (size_t i = constants_count; i > 0; i -= 1) {
        if (str_eq(nodes[constants[i - 1]].token.str, name)) {
            *index = i - 1;
            return true;
        }
    }
    return false;
}

size_t variables[SCOPE_CAP];
size_t variables_count;

void variables_push(size_t node)
{
    assert(variables_count < SCOPE_CAP);
    variables[variables_count] = node;
    variables_count += 1;
}

bool variables_find(Str name, size_t *index)
{
    for (size_t i = variables_count; i > 0; i -= 1) {
        if (str_eq(nodes[variables[i - 1]].token.str, name)) {
            *index = i - 1;
            return true;
        }
    }
    return false;
}

typedef struct {
    size_t node;
    size_t args;
    size_t vars;

    size_t ret;
    size_t arity;
} Function;

Function functions[SCOPE_CAP];
size_t functions_count;
size_t functions_current;

void functions_push(size_t node, size_t arity)
{
    assert(functions_count < SCOPE_CAP);
    functions[functions_count].node = node;
    functions[functions_count].arity = arity;
    functions_count += 1;
}

bool functions_find(Str name, size_t *index)
{
    for (size_t i = 0; i < functions_count; i += 1) {
        if (str_eq(nodes[functions[i].node].token.str, name)) {
            *index = i;
            return true;
        }
    }
    return false;
}

size_t structures[SCOPE_CAP];
size_t structures_count;

void structures_push(size_t node)
{
    assert(structures_count < SCOPE_CAP);
    structures[structures_count] = node;
    structures_count += 1;
}

bool structures_find(Str name, size_t *index)
{
    for (size_t i = structures_count; i > 0; i -= 1) {
        if (str_eq(nodes[structures[i - 1]].token.str, name)) {
            *index = i - 1;
            return true;
        }
    }
    return false;
}

// Checker
void error_undefined(size_t node, char *name)
{
    print_pos(stderr, nodes[node].token.pos);
    fprintf(stderr, "error: undefined %s '", name);
    print_str(stderr, nodes[node].token.str);
    fprintf(stderr, "'\n");
    exit(1);
}

void error_redefinition(size_t node, size_t prev, char *name)
{
    print_pos(stderr, nodes[node].token.pos);
    fprintf(stderr, "error: redefinition of %s '", name);
    print_str(stderr, nodes[node].token.str);
    fprintf(stderr, "'\n");
    print_pos(stderr, nodes[prev].token.pos);
    fprintf(stderr, "note: defined here\n");
    exit(1);
}

void check_redefinition(size_t node, size_t list, char *name)
{
    while (list != 0 && list != node) {
        if (str_eq(nodes[list].token.str, nodes[node].token.str)) {
            error_redefinition(node, list, name);
        }
        list = nodes[list].next;
    }
}

void ref_prevent(size_t node, bool ref)
{
    if (ref) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: cannot take reference to value not in memory\n");
        exit(1);
    }
}

Type type_assert(size_t node, Type expected)
{
    Type actual = nodes[node].type;
    if (!type_eq(actual, expected)) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected type '");
        print_type(stderr, expected);
        fprintf(stderr, "', got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

Type type_assert_arith(size_t node)
{
    Type actual = nodes[node].type;
    if (actual.kind != TYPE_INT && actual.kind != TYPE_CHAR && actual.ref == 0) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected arithmetic type, got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

Type type_assert_scalar(size_t node)
{
    Type actual = nodes[node].type;
    if (actual.kind != TYPE_INT && actual.kind != TYPE_BOOL && actual.kind != TYPE_CHAR && actual.ref == 0) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected scalar type, got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

Type type_assert_pointer(size_t node)
{
    Type actual = nodes[node].type;
    if (actual.ref == 0 && actual.kind != TYPE_ARRAY) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected pointer type, got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void check_const(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        switch (nodes[node].token.kind) {
        case TOKEN_INT:
            nodes[node].type = type_new(TYPE_INT, 0, 0);
            break;

        case TOKEN_BOOL:
            nodes[node].type = type_new(TYPE_BOOL, 0, 0);
            break;

        case TOKEN_CHAR:
            nodes[node].type = type_new(TYPE_CHAR, 0, 0);
            break;

        case TOKEN_IDENT: {
            size_t index;
            if (constants_find(nodes[node].token.str, &index)) {
                nodes[node].type = nodes[constants[index]].type;
                nodes[node].token.data = nodes[constants[index]].token.data;
            } else {
                error_undefined(node, "constant");
            }
        } break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_UNARY: {
        size_t expr = nodes[node].nodes[NODE_UNARY_EXPR];

        switch (nodes[node].token.kind) {
        case TOKEN_SUB:
        case TOKEN_BNOT:
            check_const(expr);
            nodes[node].type = type_assert_arith(expr);
            break;

        case TOKEN_LNOT:
            check_const(expr);
            nodes[node].type = type_assert(expr, type_new(TYPE_BOOL, 0, 0));
            break;

        default: assert(0 && "unreachable");
        }

        eval_const_unary(node);
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
        case TOKEN_BOR:
        case TOKEN_BAND:
            check_const(lhs);
            check_const(rhs);
            nodes[node].type = type_assert(rhs, type_assert_arith(lhs));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            check_const(lhs);
            check_const(rhs);
            type_assert(rhs, type_assert_arith(lhs));
            nodes[node].type = type_new(TYPE_BOOL, 0, 0);
            break;

        default: assert(0 && "unreachable");
        }

        eval_const_binary(node);
    } break;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_TYPES == 6);
void check_type(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        if (str_eq(nodes[node].token.str, str_from_cstr("int"))) {
            nodes[node].type = type_new(TYPE_INT, 0, 0);
        } else if (str_eq(nodes[node].token.str, str_from_cstr("bool"))) {
            nodes[node].type = type_new(TYPE_BOOL, 0, 0);
        } else if (str_eq(nodes[node].token.str, str_from_cstr("char"))) {
            nodes[node].type = type_new(TYPE_CHAR, 0, 0);
        } else if (structures_find(nodes[node].token.str, &nodes[node].token.data)) {
            nodes[node].token.data = structures[nodes[node].token.data];
            nodes[node].type = type_new(TYPE_STRUCT, 0, nodes[node].token.data);
        } else {
            error_undefined(node, "type");
        }
        break;

    case NODE_UNARY: {
        size_t expr = nodes[node].nodes[NODE_UNARY_EXPR];
        check_type(expr);
        nodes[node].type = type_ref(nodes[expr].type);
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        check_const(lhs);
        type_assert(lhs, type_new(TYPE_INT, 0, 0));
        if (nodes[lhs].token.data == 0) {
            print_pos(stderr, nodes[lhs].token.pos);
            fprintf(stderr, "error: array cannot have zero elements\n");
            exit(1);
        }

        check_type(rhs);
        nodes[node].type = type_new(TYPE_ARRAY, 0, node);
    } break;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void check_expr(size_t node, bool ref)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        switch (nodes[node].token.kind) {
        case TOKEN_INT:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_INT, 0, 0);
            break;

        case TOKEN_STR:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_STRUCT, 0, 0);
            break;

        case TOKEN_ARGC:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_INT, 0, 0);
            break;

        case TOKEN_ARGV:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_CHAR, 2, 0);
            break;

        case TOKEN_BOOL:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_BOOL, 0, 0);
            break;

        case TOKEN_CHAR:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_CHAR, 0, 0);
            break;

        case TOKEN_CSTR:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_CHAR, 1, 0);
            break;

        case TOKEN_IDENT: {
            size_t index;
            if (variables_find(nodes[node].token.str, &index)) {
                nodes[node].type = nodes[variables[index]].type;
                nodes[node].token.data = variables[index];
            } else if (constants_find(nodes[node].token.str, &index)) {
                ref_prevent(node, ref);
                nodes[node].type = nodes[constants[index]].type;
                nodes[node].token.data = nodes[constants[index]].token.data;
                nodes[node].token.kind = TOKEN_INT;
            } else {
                error_undefined(node, "identifier");
            }
        } break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_CALL:
        ref_prevent(node, ref);

        if (str_eq(nodes[node].token.str, str_from_cstr("main"))) {
            print_pos(stderr, nodes[node].token.pos);
            fprintf(stderr, "error: function 'main' cannot be called\n");
            exit(1);
        }

        if (str_eq(nodes[node].token.str, str_from_cstr("syscall"))) {
            if (nodes[node].token.data < 1 || nodes[node].token.data > 7) {
                print_pos(stderr, nodes[node].token.pos);
                fprintf(stderr, "error: expected 1 to 7 arguments, got %zu\n", nodes[node].token.data);
                exit(1);
            }

            for (size_t call = nodes[node].nodes[NODE_CALL_ARGS]; call != 0; call = nodes[call].next) {
                check_expr(call, false);
                type_assert_scalar(call);
            }

            nodes[node].type = type_new(TYPE_INT, 0, 0);
        } else {
            size_t index;
            if (!functions_find(nodes[node].token.str, &index)) {
                error_undefined(node, "function");
            }

            if (nodes[node].token.data != functions[index].arity) {
                print_pos(stderr, nodes[node].token.pos);
                fprintf(stderr, "error: expected %zu arguments, got %zu\n",
                        functions[index].arity, nodes[node].token.data);
                print_pos(stderr, nodes[functions[index].node].token.pos);
                fprintf(stderr, "note: defined here\n");
                exit(1);
            }

            size_t call = nodes[node].nodes[NODE_CALL_ARGS];
            size_t real = nodes[functions[index].node].nodes[NODE_FN_ARGS];
            while (real != 0) {
                check_expr(call, false);
                type_assert(call, nodes[real].type);

                call = nodes[call].next;
                real = nodes[real].next;
            }

            nodes[node].token.data = index;
            nodes[node].type = nodes[functions[index].node].type;
        }
        break;

    case NODE_UNARY: {
        size_t expr = nodes[node].nodes[NODE_UNARY_EXPR];

        switch (nodes[node].token.kind) {
        case TOKEN_SUB:
        case TOKEN_BNOT:
            ref_prevent(node, ref);
            check_expr(expr, false);
            nodes[node].type = type_assert_arith(expr);
            break;

        case TOKEN_LNOT:
            ref_prevent(node, ref);
            check_expr(expr, false);
            nodes[node].type = type_assert(expr, type_new(TYPE_BOOL, 0, 0));
            break;

        case TOKEN_MUL:
            check_expr(expr, false);
            if (type_isarray(nodes[expr].type)) {
                print_pos(stderr, nodes[expr].token.pos);
                fprintf(stderr, "error: cannot deference array\n");
                exit(1);
            }
            nodes[node].type = type_deref(type_assert_pointer(expr));
            break;

        case TOKEN_BAND:
            ref_prevent(node, ref);
            check_expr(expr, true);
            nodes[node].type = type_ref(nodes[expr].type);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.kind) {
        case TOKEN_DOT:
            check_expr(lhs, false);
            if (nodes[lhs].type.kind != TYPE_STRUCT) {
                print_pos(stderr, nodes[lhs].token.pos);
                fprintf(stderr, "error: expected structure value, got '");
                print_type(stderr, nodes[lhs].type);
                fprintf(stderr, "'\n");
                exit(1);
            }

            size_t real = nodes[lhs].type.data;
            if (!node_list_find(nodes[real].nodes[NODE_STRUCT_FIELDS], rhs)) {
                print_pos(stderr, nodes[rhs].token.pos);
                fprintf(stderr, "error: undefined field '");
                print_str(stderr, nodes[rhs].token.str);
                fprintf(stderr, "' in structure '");
                print_str(stderr, nodes[real].token.str);
                fprintf(stderr, "'\n");

                if (nodes[real].token.pos.path != NULL) {
                    print_pos(stderr, nodes[real].token.pos);
                    fprintf(stderr, "note: defined here\n");
                    exit(1);
                }
            }

            nodes[node].type = nodes[nodes[rhs].token.data].type;
            break;

        case TOKEN_LBRACKET:
            check_expr(lhs, false);
            ref_prevent(lhs, nodes[lhs].kind == NODE_CALL && type_isarray(nodes[lhs].type));
            check_expr(rhs, false);
            nodes[node].type = type_deref(type_assert_pointer(lhs));
            type_assert(rhs, type_new(TYPE_INT, 0, 0));
            break;

        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_MOD:
        case TOKEN_BOR:
        case TOKEN_BAND:
            ref_prevent(node, ref);
            check_expr(lhs, false);
            check_expr(rhs, false);
            nodes[node].type = type_assert(rhs, type_assert_arith(lhs));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            ref_prevent(node, ref);
            check_expr(lhs, false);
            check_expr(rhs, false);
            type_assert(rhs, type_assert_arith(lhs));
            nodes[node].type = type_new(TYPE_BOOL, 0, 0);
            break;

        case TOKEN_SET:
            ref_prevent(node, ref);
            check_expr(lhs, true);
            check_expr(rhs, false);
            type_assert(rhs, nodes[lhs].type);
            nodes[node].type = type_new(TYPE_NIL, 0, 0);
            break;

        case TOKEN_ADD_SET:
        case TOKEN_SUB_SET:
        case TOKEN_MUL_SET:
        case TOKEN_DIV_SET:
        case TOKEN_MOD_SET:
            ref_prevent(node, ref);
            check_expr(lhs, true);
            type_assert_arith(lhs);

            check_expr(rhs, false);
            type_assert(rhs, nodes[lhs].type);
            nodes[node].type = type_new(TYPE_NIL, 0, 0);
            break;

        case TOKEN_AS: {
            ref_prevent(node, ref);

            check_expr(lhs, false);
            type_assert_scalar(lhs);

            check_type(rhs);
            type_assert_scalar(rhs);

            nodes[node].type = nodes[rhs].type;
        } break;

        default: assert(0 && "unreachable");
        }
    } break;

    default: assert(0 && "unreachable");
    }
}

typedef struct {
    size_t node;
    size_t value;
} Pred;

Pred preds[SCOPE_CAP];
size_t preds_count;

void preds_push(size_t node)
{
    assert(preds_count < SCOPE_CAP);
    preds[preds_count].node = node;
    preds[preds_count].value = nodes[node].token.data;
    preds_count += 1;
}

bool preds_find(size_t value, size_t start, size_t *index)
{
    for (size_t i = start; i < preds_count; i += 1) {
        if (preds[i].value == value) {
            *index = i;
            return true;
        }
    }
    return false;
}

static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void check_stmt(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_BLOCK: {
        size_t constants_count_save = constants_count;
        size_t variables_count_save = variables_count;
        for (node = nodes[node].nodes[NODE_BLOCK_START]; node != 0; node = nodes[node].next) {
            check_stmt(node);
        }
        constants_count = constants_count_save;
        variables_count = variables_count_save;
    } break;

    case NODE_IF: {
        size_t cond = nodes[node].nodes[NODE_IF_COND];
        check_expr(cond, false);
        type_assert(cond, type_new(TYPE_BOOL, 0, 0));

        check_stmt(nodes[node].nodes[NODE_IF_THEN]);

        size_t ante = nodes[node].nodes[NODE_IF_ELSE];
        if (ante != 0) {
            check_stmt(ante);
        }
    } break;

    case NODE_FOR: {
        size_t variables_count_save = variables_count;

        size_t init = nodes[node].nodes[NODE_FOR_INIT];
        if (init != 0) {
            check_stmt(init);
        }

        size_t cond = nodes[node].nodes[NODE_FOR_COND];
        check_expr(cond, false);
        type_assert(cond, type_new(TYPE_BOOL, 0, 0));

        size_t update = nodes[node].nodes[NODE_FOR_UPDATE];
        if (update != 0) {
            check_stmt(update);
        }

        check_stmt(nodes[node].nodes[NODE_FOR_BODY]);
        variables_count = variables_count_save;
    } break;

    case NODE_MATCH: {
        size_t preds_count_save = preds_count;

        size_t expr = nodes[node].nodes[NODE_MATCH_EXPR];
        check_expr(expr, false);
        type_assert_scalar(expr);

        for (size_t branch = nodes[node].nodes[NODE_MATCH_LIST]; branch != 0; branch = nodes[branch].next) {
            for (size_t pred = nodes[branch].nodes[NODE_BRANCH_LIST]; pred != 0; pred = nodes[pred].next) {
                check_const(pred);
                type_assert(pred, nodes[expr].type);

                size_t prev;
                if (preds_find(nodes[pred].token.data, preds_count_save, &prev)) {
                    print_pos(stderr, nodes[pred].token.pos);
                    fprintf(stderr, "error: duplicate branch '%zu'\n", nodes[pred].token.data);

                    print_pos(stderr, nodes[preds[prev].node].token.pos);
                    fprintf(stderr, "note: handled here\n");
                    exit(1);
                }

                preds_push(pred);
            }
            check_stmt(nodes[branch].nodes[NODE_BRANCH_BODY]);
        }

        preds_count = preds_count_save;

        if (nodes[node].nodes[NODE_MATCH_ELSE] != 0) {
            check_stmt(nodes[node].nodes[NODE_MATCH_ELSE]);
        }
    } break;

    case NODE_BRANCH:
        break;

    case NODE_FN: {
        size_t prev;
        if (functions_find(nodes[node].token.str, &prev)) {
            error_redefinition(node, functions[prev].node, "function");
        }

        size_t arity = 0;
        size_t variables_count_save = variables_count;
        size_t args = nodes[node].nodes[NODE_FN_ARGS];
        for (size_t arg = args; arg != 0; arg = nodes[arg].next) {
            check_redefinition(arg, args, "argument");
            check_stmt(arg);
            arity += 1;
        }

        if (str_eq(nodes[node].token.str, str_from_cstr("main"))) {
            if (arity != 0) {
                print_pos(stderr, nodes[nodes[node].nodes[NODE_FN_ARGS]].token.pos);
                fprintf(stderr, "error: function 'main' cannot take arguments\n");
                exit(1);
            }
        }

        size_t ret = nodes[node].nodes[NODE_FN_TYPE];
        if (ret != 0) {
            check_type(ret);
            nodes[node].type = nodes[ret].type;
        } else {
            nodes[node].type = type_new(TYPE_NIL, 0, 0);
        }

        nodes[node].token.data = functions_count;

        functions_current = functions_count;
        functions_push(node, arity);

        check_stmt(nodes[node].nodes[NODE_FN_BODY]);
        variables_count = variables_count_save;
    } break;

    case NODE_LET: {
        size_t expr = nodes[node].nodes[NODE_LET_EXPR];
        if (expr != 0) {
            check_expr(expr, false);
            if (type_eq(nodes[expr].type, type_new(TYPE_NIL, 0, 0))) {
                print_pos(stderr, nodes[expr].token.pos);
                fprintf(stderr, "error: cannot declare variable with type 'nil'\n");
                exit(1);
            }
            nodes[node].type = nodes[expr].type;
        } else {
            expr = nodes[node].nodes[NODE_LET_TYPE];
            check_type(expr);
            nodes[node].type = nodes[expr].type;
        }

        variables_push(node);
    } break;

    case NODE_CONST: {
        size_t list = nodes[node].nodes[NODE_CONST_LIST];
        if (list != 0) {
            for (size_t i = 0; list != 0; i += 1) {
                nodes[list].type = type_new(TYPE_INT, 0, 0);
                nodes[list].token.data = i;
                constants_push(list);
                list = nodes[list].next;
            }
        } else {
            size_t expr = nodes[node].nodes[NODE_CONST_EXPR];
            check_const(expr);
            nodes[node].type = nodes[expr].type;
            nodes[node].token.data = nodes[expr].token.data;
            constants_push(node);
        }
    } break;

    case NODE_STRUCT: {
        size_t prev;
        if (structures_find(nodes[node].token.str, &prev)) {
            error_redefinition(node, structures[prev], "structure");
        }

        if (str_eq(nodes[node].token.str, str_from_cstr("nil")) ||
            str_eq(nodes[node].token.str, str_from_cstr("int")) ||
            str_eq(nodes[node].token.str, str_from_cstr("bool")))
        {
            print_pos(stderr, nodes[node].token.pos);
            fprintf(stderr, "error: redefinition of builtin type '");
            print_str(stderr, nodes[node].token.str);
            fprintf(stderr, "'\n");
            exit(1);
        }

        size_t fields = nodes[node].nodes[NODE_FN_ARGS];
        for (size_t field = fields; field != 0; field = nodes[field].next) {
            check_redefinition(field, fields, "field");
            size_t type = nodes[field].nodes[NODE_LET_TYPE];
            check_type(type);
            nodes[field].type = nodes[type].type;
        }

        structures_push(node);
    } break;

    case NODE_ASSERT: {
        size_t expr = nodes[node].nodes[NODE_ASSERT_EXPR];
        if (nodes[node].token.data == 1) {
            check_expr(expr, false);
        } else {
            check_const(expr);
        }

        type_assert(expr, type_new(TYPE_BOOL, 0, 0));

        if (nodes[node].token.data == 0 && nodes[expr].token.data == 0) {
            print_pos(stderr, nodes[node].token.pos);
            fprintf(stderr, "assertion failed\n");
            exit(1);
        }
    } break;

    case NODE_RETURN: {
        size_t expr = nodes[node].nodes[NODE_RETURN_EXPR];
        if (expr != 0) {
            check_expr(expr, false);
            nodes[node].type = nodes[expr].type;
        } else {
            nodes[node].type = type_new(TYPE_NIL, 0, 0);
        }
        type_assert(node, nodes[functions[functions_current].node].type);
    } break;

    case NODE_PRINT: {
        size_t expr = nodes[node].nodes[NODE_PRINT_EXPR];
        check_expr(expr, false);
        type_assert_scalar(expr);
    } break;

    default: check_expr(node, false);
    }
}

// Op
enum {
    OP_PUSH,
    OP_DROP,
    OP_DUP,
    OP_STR,
    OP_CSTR,

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,

    OP_BOR,
    OP_BAND,
    OP_BNOT,
    OP_LNOT,

    OP_GT,
    OP_GE,
    OP_LT,
    OP_LE,
    OP_EQ,
    OP_NE,

    OP_GPTR,
    OP_LPTR,
    OP_LOAD,
    OP_STORE,

    OP_FIELD,
    OP_INDEX,

    OP_GOTO,
    OP_ELSE,
    OP_THEN,

    OP_CALL,
    OP_RET,

    OP_ARGC,
    OP_ARGV,
    OP_HALT,
    OP_SYSCALL,

    OP_PRINT,
    COUNT_OPS
};

typedef struct {
    int kind;
    size_t data;
} Op;

static_assert(COUNT_OPS == 37);
void print_op(FILE *file, Op op)
{
    switch (op.kind) {
    case OP_PUSH:
        fprintf(file, "push %zu\n", op.data);
        break;

    case OP_DROP:
        fprintf(file, "drop %zu\n", op.data);
        break;

    case OP_DUP:
        fprintf(file, "dup\n");
        break;

    case OP_STR:
        fprintf(file, "str %zu\n", op.data);
        break;

    case OP_CSTR:
        fprintf(file, "cstr %zu\n", op.data);
        break;

    case OP_ADD:
        fprintf(file, "add\n");
        break;

    case OP_SUB:
        fprintf(file, "sub\n");
        break;

    case OP_MUL:
        fprintf(file, "mul\n");
        break;

    case OP_DIV:
        fprintf(file, "div\n");
        break;

    case OP_MOD:
        fprintf(file, "mod\n");
        break;

    case OP_NEG:
        fprintf(file, "neg\n");
        break;

    case OP_BOR:
        fprintf(file, "bor\n");
        break;

    case OP_BAND:
        fprintf(file, "band\n");
        break;

    case OP_BNOT:
        fprintf(file, "bnot\n");
        break;

    case OP_LNOT:
        fprintf(file, "lnot\n");
        break;

    case OP_GT:
        fprintf(file, "gt\n");
        break;

    case OP_GE:
        fprintf(file, "ge\n");
        break;

    case OP_LT:
        fprintf(file, "lt\n");
        break;

    case OP_LE:
        fprintf(file, "le\n");
        break;

    case OP_EQ:
        fprintf(file, "eq\n");
        break;

    case OP_NE:
        fprintf(file, "ne\n");
        break;

    case OP_GPTR:
        fprintf(file, "gptr %zu\n", op.data);
        break;

    case OP_LPTR:
        fprintf(file, "lptr %zu\n", op.data);
        break;

    case OP_LOAD:
        fprintf(file, "load %zu\n", op.data);
        break;

    case OP_STORE:
        fprintf(file, "store %zu\n", op.data);
        break;

    case OP_FIELD:
        fprintf(file, "field %zu\n", op.data);
        break;

    case OP_INDEX:
        fprintf(file, "index %zu\n", op.data);
        break;

    case OP_GOTO:
        fprintf(file, "goto %zu\n", op.data);
        break;

    case OP_ELSE:
        fprintf(file, "else %zu\n", op.data);
        break;

    case OP_THEN:
        fprintf(file, "then %zu\n", op.data);
        break;

    case OP_CALL: {
        Function *function = &functions[op.data];
        fprintf(file, "call addr=%zu, args=%zu, vars=%zu\n", nodes[function->node].token.data, function->args, function->vars);
    } break;

    case OP_RET: {
        Function *function = &functions[op.data];
        fprintf(file, "ret args=%zu, vars=%zu size=%zu\n", function->args, function->vars, function->ret);
    } break;

    case OP_ARGC:
        fprintf(file, "argc\n");
        break;

    case OP_ARGV:
        fprintf(file, "argv\n");
        break;

    case OP_HALT:
        fprintf(file, "halt\n");
        break;

    case OP_SYSCALL:
        fprintf(file, "syscall %zu\n", op.data);
        break;

    case OP_PRINT:
        fprintf(file, "print\n");
        break;

    default: assert(0 && "unreachable");
    }
}

#define OPS_CAP 1024
Op ops[OPS_CAP];
size_t ops_count;

void ops_push(int kind, size_t data)
{
    assert(ops_count < OPS_CAP);
    ops[ops_count].kind = kind;
    ops[ops_count].data = data;
    ops_count += 1;
}

void print_ops(FILE *file)
{
    for (size_t i = 0; i < ops_count; i += 1) {
        fprintf(file, "%04zu ", i);
        print_op(file, ops[i]);
    }
}

// String Literals
#define STRS_CAP 1024

Str strs[STRS_CAP];
size_t strs_count;
Arena strs_arena;

size_t str_push(Str str)
{
    assert(strs_count < STRS_CAP);
    strs[strs_count] = str;
    strs_count += 1;
    return strs_count - 1;
}

Str token_str_encode(Token token, bool cstr)
{
    if (token.kind == TOKEN_STR) {
        token.str.size -= 1;
    } else {
        token.str.size -= 2;
    }

    Str str;
    str.data = strs_arena.data + strs_arena.size;
    str.size = 0;

    for (size_t i = 1; i < token.str.size; i += 1) {
        char ch = token.str.data[i];
        if (ch == '\\') {
            i += 1;
            switch (token.str.data[i]) {
            case 'n':
                ch = '\n';
                break;

            case 't':
                ch = '\t';
                break;

            case '0':
                ch = '\0';
                break;

            case '"':
                ch = '"';
                break;

            case '\'':
                ch = '\'';
                break;

            case '\\':
                ch = '\\';
                break;

            default: assert(0 && "unreachable");
            }
        }

        arena_push_char(&strs_arena, ch);
        str.size += 1;
    }

    if (cstr) {
        arena_push_char(&strs_arena, '\0');
    }

    return str;
}


// Compiler
size_t align(size_t n)
{
    return (n + 7) & -8;
}

static_assert(COUNT_TYPES == 6);
size_t type_size(Type type)
{
    if (type.ref > 0) {
        return 8;
    }

    switch (type.kind) {
    case TYPE_NIL:
        return 0;

    case TYPE_INT:
        return 8;

    case TYPE_BOOL:
    case TYPE_CHAR:
        return 1;

    case TYPE_ARRAY: {
        size_t base = type_size(nodes[nodes[type.data].nodes[NODE_BINARY_RHS]].type);
        size_t count = nodes[nodes[type.data].nodes[NODE_BINARY_LHS]].token.data;
        return base * count;
    }

    case TYPE_STRUCT:
        return nodes[type.data].token.data;

    default: assert(0 && "unreachable");
    }
}

size_t local_max;
size_t local_size;
size_t local_alloc(size_t size)
{
    local_size += size;
    return local_size;
}

size_t global_size;
size_t global_alloc(size_t size)
{
    global_size += size;
    return global_size - size;
}

void compile_ref(size_t node)
{
    if (nodes[node].token.data | 1) {
        ops_push(OP_LPTR, nodes[node].token.data & ~1);
    } else {
        ops_push(OP_GPTR, nodes[node].token.data);
    }
}

static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void compile_expr(size_t node, bool ref)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        switch (nodes[node].token.kind) {
        case TOKEN_ARGC:
            ops_push(OP_ARGC, 0);
            break;

        case TOKEN_ARGV:
            ops_push(OP_ARGV, 0);
            break;

        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            ops_push(OP_PUSH, nodes[node].token.data);
            break;

        case TOKEN_STR:
            ops_push(OP_STR, str_push(token_str_encode(nodes[node].token, false)));
            break;

        case TOKEN_CSTR:
            ops_push(OP_CSTR, str_push(token_str_encode(nodes[node].token, true)));
            break;

        case TOKEN_IDENT: {
            size_t real = nodes[node].token.data;
            compile_ref(real);

            if (!ref) {
                ops_push(OP_LOAD, type_size(nodes[node].type));
            }
        } break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_CALL:
        for (size_t arg = nodes[node].nodes[NODE_CALL_ARGS]; arg != 0; arg = nodes[arg].next) {
            compile_expr(arg, false);
        }

        if (str_eq(nodes[node].token.str, str_from_cstr("syscall"))) {
            ops_push(OP_SYSCALL, nodes[node].token.data);
        } else {
            ops_push(OP_CALL, nodes[node].token.data);
        }
        break;

    case NODE_UNARY: {
        size_t expr = nodes[node].nodes[NODE_UNARY_EXPR];

        switch (nodes[node].token.kind) {
        case TOKEN_SUB:
            compile_expr(expr, false);
            ops_push(OP_NEG, 0);
            break;

        case TOKEN_MUL:
            compile_expr(expr, false);
            if (!ref) {
                ops_push(OP_LOAD, type_size(nodes[node].type));
            }
            break;

        case TOKEN_BAND:
            compile_expr(expr, true);
            break;

        case TOKEN_BNOT:
            compile_expr(expr, false);
            ops_push(OP_BNOT, 0);
            break;

        case TOKEN_LNOT:
            compile_expr(expr, false);
            ops_push(OP_LNOT, 0);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.kind) {
        case TOKEN_DOT:
            compile_expr(lhs, true);
            for (size_t i = 0; i < nodes[lhs].type.ref; i += 1) {
                ops_push(OP_LOAD, 8);
            }

            ops_push(OP_FIELD, nodes[nodes[rhs].token.data].token.data);
            if (!ref) {
                ops_push(OP_LOAD, type_size(nodes[node].type));
            }
            break;

        case TOKEN_LBRACKET: {
            bool array = type_isarray(nodes[lhs].type);
            compile_expr(lhs, array);
            compile_expr(rhs, false);

            size_t size = type_size(nodes[node].type);
            ops_push(OP_INDEX, size);

            if (!ref) {
                ops_push(OP_LOAD, size);
            }
            break;
        }

        case TOKEN_ADD:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_ADD, 0);
            break;

        case TOKEN_SUB:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_SUB, 0);
            break;

        case TOKEN_MUL:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_MUL, 0);
            break;

        case TOKEN_DIV:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_DIV, 0);
            break;

        case TOKEN_MOD:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_MOD, 0);
            break;

        case TOKEN_BOR:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_BOR, 0);
            break;

        case TOKEN_BAND:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_BAND, 0);
            break;

        case TOKEN_SET:
            compile_expr(lhs, true);
            compile_expr(rhs, false);
            ops_push(OP_STORE, type_size(nodes[rhs].type));
            break;

        case TOKEN_ADD_SET: {
            size_t size = type_size(nodes[rhs].type);
            compile_expr(lhs, true);
            ops_push(OP_DUP, 0);
            ops_push(OP_LOAD, size);
            compile_expr(rhs, false);
            ops_push(OP_ADD, 0);
            ops_push(OP_STORE, size);
        } break;

        case TOKEN_SUB_SET: {
            size_t size = type_size(nodes[rhs].type);
            compile_expr(lhs, true);
            ops_push(OP_DUP, 0);
            ops_push(OP_LOAD, size);
            compile_expr(rhs, false);
            ops_push(OP_SUB, 0);
            ops_push(OP_STORE, size);
        } break;

        case TOKEN_MUL_SET: {
            size_t size = type_size(nodes[rhs].type);
            compile_expr(lhs, true);
            ops_push(OP_DUP, 0);
            ops_push(OP_LOAD, size);
            compile_expr(rhs, false);
            ops_push(OP_MUL, 0);
            ops_push(OP_STORE, size);
        } break;

        case TOKEN_DIV_SET: {
            size_t size = type_size(nodes[rhs].type);
            compile_expr(lhs, true);
            ops_push(OP_DUP, 0);
            ops_push(OP_LOAD, size);
            compile_expr(rhs, false);
            ops_push(OP_DIV, 0);
            ops_push(OP_STORE, size);
        } break;

        case TOKEN_MOD_SET: {
            size_t size = type_size(nodes[rhs].type);
            compile_expr(lhs, true);
            ops_push(OP_DUP, 0);
            ops_push(OP_LOAD, size);
            compile_expr(rhs, false);
            ops_push(OP_MOD, 0);
            ops_push(OP_STORE, size);
        } break;

        case TOKEN_GT:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_GT, 0);
            break;

        case TOKEN_GE:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_GE, 0);
            break;

        case TOKEN_LT:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_LT, 0);
            break;

        case TOKEN_LE:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_LE, 0);
            break;

        case TOKEN_EQ:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_EQ, 0);
            break;

        case TOKEN_NE:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_NE, 0);
            break;

        case TOKEN_AS:
            compile_expr(lhs, false);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    default:
        assert(0 && "unreachable");
    }
}

#define JUMPS_CAP 64
typedef struct {
    size_t data[JUMPS_CAP];
    size_t count;
} Jumps;

void jumps_save(Jumps *jumps)
{
    assert(jumps->count < JUMPS_CAP);
    jumps->data[jumps->count] = ops_count;
    jumps->count += 1;
}

void jumps_restore(Jumps *jumps, size_t start)
{
    for (size_t i = start; i < jumps->count; i += 1) {
        ops[jumps->data[i]].data = ops_count;
    }
    jumps->count = start;
}

Jumps pred_jumps;
Jumps branch_jumps;

static_assert(COUNT_NODES == 16);
static_assert(COUNT_TOKENS == 52);
void compile_stmt(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_BLOCK: {
        size_t local_size_save = local_size;
        for (node = nodes[node].nodes[NODE_BLOCK_START]; node != 0; node = nodes[node].next) {
            compile_stmt(node);
        }
        local_max = max(local_max, local_size);
        local_size = local_size_save;
    } break;

    case NODE_IF: {
        compile_expr(nodes[node].nodes[NODE_IF_COND], false);

        size_t then_addr = ops_count;
        ops_push(OP_ELSE, 0);
        compile_stmt(nodes[node].nodes[NODE_IF_THEN]);

        if (nodes[node].nodes[NODE_IF_ELSE] != 0) {
            size_t else_addr = ops_count;
            ops_push(OP_GOTO, 0);
            ops[then_addr].data = ops_count;
            compile_stmt(nodes[node].nodes[NODE_IF_ELSE]);
            ops[else_addr].data = ops_count;
        } else {
            ops[then_addr].data = ops_count;
        }
    } break;

    case NODE_FOR: {
        size_t local_size_save = local_size;

        size_t init = nodes[node].nodes[NODE_FOR_INIT];
        if (init != 0) {
            compile_stmt(init);
        }

        size_t loop_addr = ops_count;
        compile_expr(nodes[node].nodes[NODE_FOR_COND], false);

        size_t body_addr = ops_count;
        ops_push(OP_ELSE, 0);
        compile_stmt(nodes[node].nodes[NODE_FOR_BODY]);

        size_t update = nodes[node].nodes[NODE_FOR_UPDATE];
        if (update != 0) {
            compile_stmt(update);
        }

        ops_push(OP_GOTO, loop_addr);
        ops[body_addr].data = ops_count;

        local_max = max(local_max, local_size);
        local_size = local_size_save;
    } break;

    case NODE_MATCH: {
        size_t expr = nodes[node].nodes[NODE_MATCH_EXPR];
        compile_expr(expr, false);

        size_t branch_jumps_save = branch_jumps.count;
        for (size_t branch = nodes[node].nodes[NODE_MATCH_LIST]; branch != 0; branch = nodes[branch].next) {
            compile_stmt(branch);
        }

        if (nodes[node].nodes[NODE_MATCH_ELSE] != 0) {
            compile_stmt(nodes[node].nodes[NODE_MATCH_ELSE]);
        }

        jumps_restore(&branch_jumps, branch_jumps_save);

        size_t drop = type_size(nodes[expr].type);
        if (drop) {
            ops_push(OP_DROP, align(drop));
        }
    } break;

    case NODE_BRANCH: {
        for (size_t pred = nodes[node].nodes[NODE_BRANCH_LIST]; pred != 0; pred = nodes[pred].next) {
            ops_push(OP_DUP, 0);
            ops_push(OP_PUSH, nodes[pred].token.data);
            ops_push(OP_EQ, 0);

            jumps_save(&pred_jumps);
            ops_push(OP_THEN, 0);
        }

        size_t body_addr = ops_count;
        ops_push(OP_GOTO, 0);

        jumps_restore(&pred_jumps, 0);
        compile_stmt(nodes[node].nodes[NODE_BRANCH_BODY]);

        jumps_save(&branch_jumps);
        ops_push(OP_GOTO, 0);
        ops[body_addr].data = ops_count;
    } break;

    case NODE_FN: {
        local_max = 0;
        local_size = 0;

        functions_current = nodes[node].token.data;
        Function *function = &functions[functions_current];

        nodes[node].token.data = ops_count;

        size_t local_size_save = local_size;
        for (size_t arg = nodes[node].nodes[NODE_FN_ARGS]; arg != 0; arg = nodes[arg].next) {
            compile_stmt(arg);
        }
        function->args = local_size;
        function->ret = type_size(nodes[node].type);

        compile_stmt(nodes[node].nodes[NODE_FN_BODY]);
        if (function->ret == 0) {
            ops_push(OP_RET, functions_current);
        }

        function->vars = max(local_max, function->ret) - function->args;
        local_size = local_size_save;
    } break;

    case NODE_LET: {
        size_t size = type_size(nodes[node].type);
        if (nodes[node].token.data) {
            nodes[node].token.data = local_alloc(align(size)) | 1;
        } else {
            nodes[node].token.data = global_alloc(align(size));
        }

        size_t expr = nodes[node].nodes[NODE_LET_EXPR];
        if (expr != 0) {
            compile_ref(node);
            compile_expr(expr, false);
            ops_push(OP_STORE, size);
        }
    } break;

    case NODE_CONST:
        break;

    case NODE_ASSERT:
        if (nodes[node].token.data == 1) {
            compile_expr(nodes[node].nodes[NODE_ASSERT_EXPR], false);

            ops_push(OP_THEN, ops_count + 6);
            ops_push(OP_PUSH, 1);
            ops_push(OP_PUSH, 2);

            size_t capacity = ARENA_CAP - strs_arena.size;

            Str str;
            str.data = strs_arena.data + strs_arena.size;
            str.size = snprintf(str.data, capacity, "%s:%zu:%zu: assertion failed\n",
                                nodes[node].token.pos.path,
                                nodes[node].token.pos.row,
                                nodes[node].token.pos.col);
            assert(str.size < capacity);
            strs_arena.size += str.size;

            ops_push(OP_STR, str_push(str));
            ops_push(OP_SYSCALL, 4);
            ops_push(OP_HALT, 1);
        }
        break;

    case NODE_STRUCT:
        nodes[node].token.data = 0;
        for (size_t field = nodes[node].nodes[NODE_STRUCT_FIELDS]; field != 0; field = nodes[field].next) {
            nodes[field].token.data = nodes[node].token.data;
            nodes[node].token.data += align(type_size(nodes[field].type));
        }
        break;

    case NODE_RETURN: {
        size_t expr = nodes[node].nodes[NODE_RETURN_EXPR];
        if (expr != 0) {
            size_t size = functions[functions_current].ret;
            ops_push(OP_LPTR, align(size));
            compile_expr(expr, false);
            ops_push(OP_STORE, size);
        }
        ops_push(OP_RET, functions_current);
    } break;

    case NODE_PRINT:
        compile_expr(nodes[node].nodes[NODE_PRINT_EXPR], false);
        ops_push(OP_PRINT, 0);
        break;

    default: {
        compile_expr(node, false);

        size_t drop = type_size(nodes[node].type);
        if (drop != 0) {
            ops_push(OP_DROP, align(drop));
        }
    }
    }
}

// Generator
static_assert(COUNT_OPS == 37);
void generate(char *path)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "error: could not write file '%s'\n", path);
        exit(1);
    }

    fprintf(file, "format elf64 executable\n");
    fprintf(file, "segment readable executable\n");

    fprintf(file, "mov [argv], rsp\n");
    fprintf(file, "add qword [argv], 16\n");

    fprintf(file, "mov rax, [rsp]\n");
    fprintf(file, "dec rax\n");
    fprintf(file, "mov qword [argc], rax\n");

    for (size_t i = 0; i < ops_count; i += 1) {
        Op op = ops[i];

        fprintf(file, "I%zu: ; ", i);
        print_op(file, op);

        switch (op.kind) {
        case OP_PUSH:
            fprintf(file, "mov rax, %zu\n", op.data);
            fprintf(file, "push rax\n");
            break;

        case OP_DROP:
            fprintf(file, "add rsp, %zu\n", op.data);
            break;

        case OP_DUP:
            fprintf(file, "push qword [rsp]\n");
            break;

        case OP_STR:
            fprintf(file, "push strings+%zu\n", strs[op.data].data - strs_arena.data);
            fprintf(file, "push %zu\n", strs[op.data].size);
            break;

        case OP_CSTR:
            fprintf(file, "push strings+%zu\n", strs[op.data].data - strs_arena.data);
            break;

        case OP_ADD:
            fprintf(file, "pop rax\n");
            fprintf(file, "add [rsp], rax\n");
            break;

        case OP_SUB:
            fprintf(file, "pop rax\n");
            fprintf(file, "sub [rsp], rax\n");
            break;

        case OP_MUL:
            fprintf(file, "pop rbx\n");
            fprintf(file, "pop rax\n");
            fprintf(file, "imul rbx\n");
            fprintf(file, "push rax\n");
            break;

        case OP_DIV:
            fprintf(file, "pop rbx\n");
            fprintf(file, "pop rax\n");
            fprintf(file, "cqo\n");
            fprintf(file, "idiv rbx\n");
            fprintf(file, "push rax\n");
            break;

        case OP_MOD:
            fprintf(file, "pop rbx\n");
            fprintf(file, "pop rax\n");
            fprintf(file, "cqo\n");
            fprintf(file, "idiv rbx\n");
            fprintf(file, "push rdx\n");
            break;

        case OP_NEG:
            fprintf(file, "neg qword [rsp]\n");
            break;

        case OP_BOR:
            fprintf(file, "pop rax\n");
            fprintf(file, "or [rsp], rax\n");
            break;

        case OP_BAND:
            fprintf(file, "pop rax\n");
            fprintf(file, "and [rsp], rax\n");
            break;

        case OP_BNOT:
            fprintf(file, "not qword [rsp]\n");
            break;

        case OP_LNOT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "test rax, rax\n");
            fprintf(file, "sete bl\n");
            fprintf(file, "push rbx\n");
            break;

        case OP_GT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setg bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_GE:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setge bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_LT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setl bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_LE:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setle bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_EQ:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "sete bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_NE:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setne bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_GPTR:
            fprintf(file, "push memory+%zu\n", op.data);
            break;

        case OP_LPTR:
            fprintf(file, "lea rax, [rbp-%zu]\n", op.data);
            fprintf(file, "push rax\n");
            break;

        case OP_LOAD:
            fprintf(file, "pop rax\n");
            switch (op.data) {
            case 1:
                fprintf(file, "xor rbx, rbx\n");
                fprintf(file, "mov bl, [rax]\n");
                fprintf(file, "push rbx\n");
                break;

            case 8:
                fprintf(file, "push qword [rax]\n");
                break;

            default:
                fprintf(file, "sub rsp, %zu\n", op.data);
                for (size_t j = 0; j < op.data; j += 8) {
                    fprintf(file, "mov rbx, [rax+%zu]\n", j);
                    fprintf(file, "mov [rsp+%zu], rbx\n", j);
                }
            }
            break;

        case OP_STORE:
            switch (op.data) {
            case 1:
                fprintf(file, "pop rbx\n");
                fprintf(file, "pop rax\n");
                fprintf(file, "mov [rax], bl\n");
                break;

            case 8:
                fprintf(file, "pop rbx\n");
                fprintf(file, "pop rax\n");
                fprintf(file, "mov [rax], rbx\n");
                break;

            default:
                fprintf(file, "mov rax, [rsp+%zu]\n", op.data);
                for (size_t j = 0; j < op.data; j += 8) {
                    fprintf(file, "mov rbx, [rsp+%zu]\n", j);
                    fprintf(file, "mov [rax+%zu], rbx\n", j);
                }
                fprintf(file, "add rsp, %zu\n", op.data + 8);
            }
            break;

        case OP_FIELD:
            fprintf(file, "add qword [rsp], %zu\n", op.data);
            break;

        case OP_INDEX:
            fprintf(file, "pop rax\n");
            fprintf(file, "mov rbx, %zu\n", op.data);
            fprintf(file, "mul rbx\n");
            fprintf(file, "add [rsp], rax\n");
            break;

        case OP_GOTO:
            fprintf(file, "jmp I%zu\n", op.data);
            break;

        case OP_ELSE:
            fprintf(file, "pop rax\n");
            fprintf(file, "test rax, rax\n");
            fprintf(file, "jz I%zu\n", op.data);
            break;

        case OP_THEN:
            fprintf(file, "pop rax\n");
            fprintf(file, "test rax, rax\n");
            fprintf(file, "jnz I%zu\n", op.data);
            break;

        case OP_CALL: {
            Function *function = &functions[op.data];
            fprintf(file, "sub rsp, %zu\n", function->vars);
            fprintf(file, "push rbp\n");
            fprintf(file, "lea rbp, [rsp+%zu]\n", function->args + function->vars + 8);
            fprintf(file, "call I%zu\n", nodes[function->node].token.data);
        } break;

        case OP_RET: {
            Function *function = &functions[op.data];
            fprintf(file, "pop rax\n");
            fprintf(file, "pop rbp\n");
            fprintf(file, "add rsp, %zu\n", function->args + function->vars - function->ret);
            fprintf(file, "jmp rax\n");
        } break;

        case OP_ARGC:
            fprintf(file, "push qword [argc]\n");
            break;

        case OP_ARGV:
            fprintf(file, "push qword [argv]\n");
            break;

        case OP_HALT:
            fprintf(file, "mov rax, 60\n");
            fprintf(file, "xor rdi, rdi\n");
            fprintf(file, "syscall\n");
            break;

        case OP_SYSCALL:
            if (op.data > 6) { fprintf(file, "pop r9\n"); }
            if (op.data > 5) { fprintf(file, "pop r8\n"); }
            if (op.data > 4) { fprintf(file, "pop r10\n"); }
            if (op.data > 3) { fprintf(file, "pop rdx\n"); }
            if (op.data > 2) { fprintf(file, "pop rsi\n"); }
            if (op.data > 1) { fprintf(file, "pop rdi\n"); }
            fprintf(file, "pop rax\n");
            fprintf(file, "syscall\n");
            fprintf(file, "push rax\n");
            break;

        case OP_PRINT:
            fprintf(file, "pop rdi\n");
            fprintf(file, "call PRINT\n");
            break;

        default: assert(0 && "unreachable");
        }
    }

    fprintf(file, "PRINT:\n");
    fprintf(file, "mov r9, -3689348814741910323\n");
    fprintf(file, "sub rsp, 40\n");
    fprintf(file, "mov BYTE [rsp+31], 10\n");
    fprintf(file, "lea rcx, [rsp+30]\n");
    fprintf(file, ".L2:\n");
    fprintf(file, "mov rax, rdi\n");
    fprintf(file, "lea r8, [rsp+32]\n");
    fprintf(file, "mul r9\n");
    fprintf(file, "mov rax, rdi\n");
    fprintf(file, "sub r8, rcx\n");
    fprintf(file, "shr rdx, 3\n");
    fprintf(file, "lea rsi, [rdx+rdx*4]\n");
    fprintf(file, "add rsi, rsi\n");
    fprintf(file, "sub rax, rsi\n");
    fprintf(file, "add eax, 48\n");
    fprintf(file, "mov BYTE [rcx], al\n");
    fprintf(file, "mov rax, rdi\n");
    fprintf(file, "mov rdi, rdx\n");
    fprintf(file, "mov rdx, rcx\n");
    fprintf(file, "sub rcx, 1\n");
    fprintf(file, "cmp rax, 9\n");
    fprintf(file, "ja .L2\n");
    fprintf(file, "lea rax, [rsp+32]\n");
    fprintf(file, "mov edi, 1\n");
    fprintf(file, "sub rdx, rax\n");
    fprintf(file, "xor eax, eax\n");
    fprintf(file, "lea rsi, [rsp+32+rdx]\n");
    fprintf(file, "mov rdx, r8\n");
    fprintf(file, "mov rax, 1\n");
    fprintf(file, "syscall\n");
    fprintf(file, "add rsp, 40\n");
    fprintf(file, "ret\n");

    fprintf(file, "segment readable writeable\n");
    fprintf(file, "argv: rq 1\n");
    fprintf(file, "argc: rq 1\n");

    if (global_size != 0) {
        fprintf(file, "memory: rb %zu\n", global_size);
    }

    if (strs_arena.size != 0) {
        fprintf(file, "strings: db %d", *strs_arena.data);

        for (size_t i = 1; i < strs_arena.size; i += 1) {
            fprintf(file, ",%d", strs_arena.data[i]);
        }

        fprintf(file, "\n");
    }

    fclose(file);

    char *args[] = {"fasm", path, NULL};
    if (execute_command(args, true) == 0) {
        unlink(path);
    } else {
        fprintf(stderr, "error: could not generate executable\n");
        exit(1);
    }
}

// Tester
#define ARGS_CAP 1024
char *args[ARGS_CAP];
size_t args_count;

void args_push(char *item)
{
    assert(args_count < ARGS_CAP);
    args[args_count] = item;
    args_count += 1;
}

typedef struct {
    Str out;
    Str err;
    int exit;
    bool debug;
} Test;

typedef enum {
    RESULT_FAIL,
    RESULT_PASS,
    RESULT_SKIP,
} Result;

void test_free(Test *test)
{
    if (test->out.size != 0) {
        munmap(test->out.data, test->out.size);
        test->out.size = 0;
    }

    if (test->err.size != 0) {
        munmap(test->err.data, test->err.size);
        test->err.size = 0;
    }
}

void print_test(FILE *file, Test test)
{
    if (test.debug && args_count > 3) {
        fprintf(file, "argc: %zu\n", args_count - 3);
        for (size_t i = 3; i < args_count; i += 1) {
            fprintf(file, "arg: %zu\n", strlen(args[i]));
            fprintf(file, "%s\n", args[i]);
        }
        fprintf(file, "\n");
    }

    if (test.out.size != 0) {
        if (test.debug) {
            fprintf(file, "stdout: %zu\n", test.out.size);
        } else {
            fprintf(file, "stdout:\n");
        }
        print_str(file, test.out);
        fprintf(file, "\n");
    }

    if (test.err.size != 0) {
        if (test.debug) {
            fprintf(file, "stderr: %zu\n", test.err.size);
        } else {
            fprintf(file, "stderr:\n");
        }
        print_str(file, test.err);
        fprintf(file, "\n");
    }

    if (test.exit != 0) {
        fprintf(file, "exit: %d\n", test.exit);
    }
}

void test_parse_data(char *path, Str *contents, Str *out)
{
    Str value = str_split_by(contents, '\n');
    if (!int_from_str(value, &out->size)) {
        fprintf(stderr, "%s: error: invalid number '", path);
        print_str(stderr, value);
        fprintf(stderr, "'\n");
        exit(1);
    }

    if (contents->size <= out->size) {
        fprintf(stderr, "%s: error: not enough bytes in file\n", path);
        exit(1);
    }

    out->data = contents->data;
    *contents = str_drop_left(*contents, out->size);
}

size_t test_parse_int(char *path, Str *contents)
{
    Str number = str_split_by(contents, '\n');
    size_t value;
    if (!int_from_str(number, &value)) {
        fprintf(stderr, "%s: error: invalid number '", path);
        print_str(stderr, number);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return value;
}

bool test_file(char *program, char *path, Str contents)
{
    path_arena.size = 0;

    args_count = 0;
    args_push(program);
    args_push("-r");
    args_push(path);

    Test expected = {0};
    contents = str_drop_left(contents, 3);

    Str key = str_split_by(&contents, ' ');
    if (str_eq(key, str_from_cstr("argc:"))) {
        size_t args_count = test_parse_int(path, &contents);
        contents = str_trim_left(contents, '\n');
        key = str_split_by(&contents, ' ');

        for (size_t i = 0; i < args_count; i += 1) {
            if (!str_eq(key, str_from_cstr("arg:"))) {
                fprintf(stderr, "%s: error: not enough arguments in file\n", path);
                exit(1);
            }

            Str arg;
            test_parse_data(path, &contents, &arg);
            contents = str_trim_left(contents, '\n');
            key = str_split_by(&contents, ' ');

            args_push(path_arena.data + path_arena.size);
            arena_push(&path_arena, arg.data, arg.size);
            arena_push_char(&path_arena, '\0');
        }
    }

    if (str_eq(key, str_from_cstr("stdout:"))) {
        test_parse_data(path, &contents, &expected.out);
        contents = str_trim_left(contents, '\n');
        key = str_split_by(&contents, ' ');
    }

    if (str_eq(key, str_from_cstr("stderr:"))) {
        test_parse_data(path, &contents, &expected.err);
        contents = str_trim_left(contents, '\n');
        key = str_split_by(&contents, ' ');
    }

    if (str_eq(key, str_from_cstr("exit:"))) {
        expected.exit = test_parse_int(path, &contents);
    }

    Test actual = {0};
    actual.exit = capture_command(args, &actual.out, &actual.err);

    bool failed = actual.exit != expected.exit || !str_eq(actual.out, expected.out) || !str_eq(actual.err, expected.err);
    if (failed) {
        fprintf(stderr, "%s: fail\n\n", path);
        fprintf(stderr, "----------- Actual -----------\n");
        print_test(stderr, actual);
        fprintf(stderr, "------------------------------\n\n");
        fprintf(stderr, "---------- Expected ----------\n");
        print_test(stderr, expected);
        fprintf(stderr, "------------------------------\n\n");
    }

    test_free(&actual);
    return !failed;
}

// Main
void usage(FILE *file)
{
    fprintf(file, "usage:\n");
    fprintf(file, "  glos [FLAG] FILE\n\n");
    fprintf(file, "flags:\n");
    fprintf(file, "  -h    Print this help and exit\n");
    fprintf(file, "  -r    Run the program after compilation\n");
    fprintf(file, "  -t    Run the test for the program\n");
    fprintf(file, "  -T    Run the program and print test information\n");
}

enum {
    MODE_COM,
    MODE_RUN,
    MODE_TEST_RUN,
    MODE_TEST_CHECK,
};

int main(int argc, char **argv)
{
    int mode = MODE_COM;
    char *path = argv[1];
    if (path != NULL) {
        if (strcmp(path, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(path, "-r") == 0) {
            mode = MODE_RUN;
            path = argv[2];
        } else if (strcmp(path, "-T") == 0) {
            mode = MODE_TEST_RUN;
            path = argv[2];
        } else if (strcmp(path, "-t") == 0) {
            mode = MODE_TEST_CHECK;
            path = argv[2];
        }
    }

    switch (mode) {
    case MODE_TEST_RUN: {
        if (path == NULL) {
            usage(stderr);
            fprintf(stderr, "\nerror: file path not provided\n");
            exit(1);
        }

        path = argv[2];
        if (str_ends_with(str_from_cstr(path), str_from_cstr(".glos"))) {
            args_push(*argv);
            args_push("-r");
            args_push(path);
        } else {
            arena_push(&path_arena, "./", 2);
            arena_push(&path_arena, path, strlen(path));
            args_push(path_arena.data);
        }

        for (int i = 3; i < argc; i += 1) {
            args_push(argv[i]);
        }

        Test test = {0};
        test.exit = capture_command(args, &test.out, &test.err);
        test.debug = true;

        fprintf(stderr, "----------- Result -----------\n");
        print_test(stderr, test);
        fprintf(stderr, "------------------------------\n");
        test_free(&test);
    } break;

    case MODE_TEST_CHECK: {
        size_t total = 0;
        size_t failed = 0;
        size_t skipped = 0;
        for (int i = 2; i < argc; i += 1) {
            if (str_ends_with(str_from_cstr(argv[i]), str_from_cstr(".glos"))) {
                char *path = argv[i];
                Str contents;
                if (!read_file(&contents, path)) {
                    fprintf(stderr, "error: could not read file '%s'\n", path);
                    exit(1);
                }

                if (str_starts_with(contents, str_from_cstr("##\n"))) {
                    total += 1;
                    if (!test_file(*argv, argv[i], contents)) {
                        failed += 1;
                    }
                } else {
                    skipped += 1;
                    fprintf(stderr, "%s: note: testing information not found\n\n", path);
                }
                munmap(contents.data, contents.size);
            }
        }

        fprintf(stderr, "Total: %zu, Passed: %zu, Failed: %zu, Skipped: %zu\n", total, total - failed, failed, skipped);
    } break;

    default:
        if (path == NULL) {
            usage(stderr);
            fprintf(stderr, "\nerror: file path not provided\n");
            exit(1);
        }

        {
            Token token = {0};
            token.str = str_from_cstr("Str");
            token.data = 16;

            size_t str_struct = node_new(NODE_STRUCT, token);
            size_t *str_fields = &nodes[str_struct].nodes[NODE_STRUCT_FIELDS];

            token.str = str_from_cstr("size");
            token.data = 0;
            size_t str_size = node_new(NODE_LET, token);
            nodes[str_size].type = type_new(TYPE_INT, 0, 0);
            str_fields = node_list_push(str_fields, str_size);

            token.str = str_from_cstr("data");
            token.data = 8;
            size_t str_data = node_new(NODE_LET, token);
            nodes[str_data].type = type_new(TYPE_CHAR, 1, 0);
            str_fields = node_list_push(str_fields, str_data);

            structures_push(str_struct);
        }

        lexer_open(path);
        for (tops_iter = &tops_base; !lexer_read(TOKEN_EOF); ) {
            size_t stmt = parse_stmt();
            if (stmt != 0) {
                tops_iter = node_list_push(tops_iter, stmt);
            }
        }

        for (size_t iter = tops_base; iter != 0; iter = nodes[iter].next) {
            check_stmt(iter);
        }

        ops_push(OP_CALL, 0);
        ops_push(OP_HALT, 0);

        if (!functions_find(str_from_cstr("main"), &ops[0].data)) {
            print_pos(stderr, lexer.pos);
            fprintf(stderr, "error: function 'main' is not defined\n");
            exit(1);
        }

        for (size_t iter = tops_base; iter != 0; iter = nodes[iter].next) {
            compile_stmt(iter);
        }

        path_arena.size = 0;
        arena_push(&path_arena, "./", 2);
        arena_push(&path_arena, path, strlen(path));

        path_arena.size -= 5;
        path_arena.data[path_arena.size] = '\0';
        unlink(path_arena.data);

        arena_push(&path_arena, ".fasm", 6);
        generate(path_arena.data);

        if (mode == MODE_RUN) {
            path_arena.size -= 6;
            path_arena.data[path_arena.size] = '\0';

            args_push(path_arena.data);
            for (int i = 3; i < argc; i += 1) {
                args_push(argv[i]);
            }

            exit(execute_command(args, false));
        }
    }
}
