#include <stdarg.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "basic.h"

#define returnDefer(value)                                                                         \
    do {                                                                                           \
        result = (value);                                                                          \
        goto defer;                                                                                \
    } while (0)

// Character
bool resolveEscapeChar(char *ch) {
    switch (*ch) {
    case 'r':
        *ch = '\r';
        break;

    case 'n':
        *ch = '\n';
        break;

    case 't':
        *ch = '\t';
        break;

    case '0':
        *ch = '\0';
        break;

    case '"':
        *ch = '"';
        break;

    case '\'':
        *ch = '\'';
        break;

    case '\\':
        *ch = '\\';
        break;

    default:
        return false;
    }

    return true;
}

// String View
bool strEq(Str a, Str b) {
    return a.length == b.length && memcmp(a.data, b.data, b.length) == 0;
}

bool strMatch(Str a, const char *b) {
    return a.length == strlen(b) && memcmp(b, a.data, a.length) == 0;
}

bool strHasSuffix(Str a, Str b) {
    return a.length >= b.length && memcmp(&a.data[a.length - b.length], b.data, b.length) == 0;
}

Str strFromCstr(const char *cstr) {
    return (Str) {.data = cstr, .length = strlen(cstr)};
}

Str strStripSuffix(Str a, Str b) {
    while (strHasSuffix(a, b)) {
        a.length -= b.length;
    }
    return a;
}

// Temporary String Builder
static char   tempBuffer[16 * 1000 * 1000];
static size_t tempLength;

char *tempAlloc(size_t n) {
    assert(tempLength + n <= len(tempBuffer));
    char *result = &tempBuffer[tempLength];
    tempLength += n;
    return result;
}

void tempReset(const char *p) {
    assert(p >= tempBuffer && p < &tempBuffer[sizeof(tempBuffer)]);
    tempLength = p - tempBuffer;
}

void tempRemoveNull(void) {
    assert(tempLength);
    assert(tempBuffer[tempLength - 1] == '\0');
    tempLength--;
}

char *tempSprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    char *result = tempAlloc(n + 1);

    va_start(args, fmt);
    vsnprintf(result, n + 1, fmt, args);
    va_end(args);

    return result;
}

char *tempStrToCstr(Str s) {
    char *p = memcpy(tempAlloc(s.length + 1), s.data, s.length);
    p[s.length] = '\0';
    return p;
}

// OS
bool readFile(Str *out, const char *path) {
    char *data = NULL;
    bool  result = true;

    FILE *f = fopen(path, "r");
    if (!f) {
        returnDefer(false);
    }

    if (fseek(f, 0, SEEK_END)) {
        returnDefer(false);
    }

    const long length = ftell(f);
    if (length < 0) {
        returnDefer(false);
    }
    rewind(f);

    data = malloc(length);
    if (!data) {
        returnDefer(false);
    }

    if (fread(data, 1, length, f) != (size_t) length) {
        returnDefer(false);
    }

    out->data = data;
    out->length = length;

defer:
    if (f) {
        fclose(f);
    }

    if (!result) {
        free(data);
    }

    return result;
}

bool removeFile(const char *path) {
    return remove(path) >= 0;
}

int runCommand(const char **args) {
    const pid_t pid = fork();
    if (pid < 0) {
        return 1;
    }

    if (pid == 0) {
        execvp(*args, (char *const *) args);
        exit(1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return 1;
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return WEXITSTATUS(status);
}
