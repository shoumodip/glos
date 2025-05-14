#ifndef BASIC_H
#define BASIC_H

#include <assert.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>

#define len(a)        (sizeof(a) / sizeof(*(a)))
#define todo()        assert(false && "TODO");
#define unreachable() assert(false && "Unreachable");

bool removeFile(const char *path);

bool runCommand(const char **args);

#endif // BASIC_H
