#ifndef BASIC_H
#define BASIC_H

#include <assert.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper Macros
#define len(a)        (sizeof(a) / sizeof(*(a)))
#define todo()        assert(false && "TODO")
#define unreachable() assert(false && "Unreachable")

// String View
typedef struct {
    const char *data;
    size_t      length;
} Str;

#define StrFmt    "%.*s"
#define StrArg(s) (int) ((s).length), ((s).data)

bool strEq(Str a, Str b);
bool strMatch(Str a, const char *b);
bool strHasSuffix(Str a, Str b);

Str strFromCstr(const char *cstr);
Str strStripSuffix(Str a, Str b);

// Temporary String Builder
char *tempSprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *tempStrToCstr(Str s);

// OS
bool readFile(Str *out, const char *path);
bool removeFile(const char *path);
int  runCommand(const char **args);

#endif // BASIC_H
