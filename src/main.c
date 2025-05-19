#include <unistd.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "compiler.h"
#include "parser.h"

static void usage(FILE *file) {
    fprintf(file, "Usage:\n");
    fprintf(file, "    glos [FLAGS] FILE\n");
    fprintf(file, "\n");
    fprintf(file, "Flags:\n");
    fprintf(file, "    -h          Show this message\n");
    fprintf(file, "    -r          Run the program after compiling it\n");
    fprintf(file, "    -o <name>   Set the name of the output executable\n");
}

static const char *nextArg(int *argc, char ***argv, const char *expected) {
    if (*argc <= 0) {
        fprintf(stderr, "ERROR: %s not provided\n", expected);
        fprintf(stderr, "\n");
        usage(stderr);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

int main(int argc, char **argv) {
    bool        run = false;
    const char *inputPath = NULL;
    const char *outputPath = NULL;

    nextArg(&argc, &argv, "Program name");
    while (true) {
        const char *arg = nextArg(&argc, &argv, "Input file");
        if (!strcmp(arg, "-h")) {
            usage(stdout);
            exit(0);
        }

        if (!strcmp(arg, "-r")) {
            run = true;
            continue;
        }

        if (!strcmp(arg, "-o")) {
            outputPath = nextArg(&argc, &argv, "Output file");
            continue;
        }

        if (*arg == '-') {
            fprintf(stderr, "ERROR: Invalid flag '%s'\n", arg);
            fprintf(stderr, "\n");
            usage(stderr);
            exit(1);
        }

        inputPath = arg;
        break;
    }

    Lexer l = {0};
    if (!lexerNew(&l, inputPath)) {
        fprintf(stderr, "ERROR: Could not open file '%s'\n", inputPath);
        exit(1);
    }

    NodeAlloc alloc = {0};

    Parser p = {.nodeAlloc = &alloc};
    parseFile(&p, l);

    Context c = {.nodeAlloc = &alloc};
    checkNodes(&c, p.nodes);

    if (run) {
        char tmpPath[] = "/tmp/glos_run_XXXXXX";

        const char *exePath = outputPath;
        if (!outputPath) {
            const int fd = mkstemp(tmpPath);
            if (fd < 0) {
                // Could not get temporary file path. Use the input file path as template
                const Str base = strStripSuffix(strFromCstr(inputPath), strFromCstr(".glos"));
                if (*base.data == '/') {
                    exePath = tempStrToCstr(base);
                } else {
                    exePath = tempSprintf("./" StrFmt, StrArg(base));
                }
            } else {
                close(fd);
                exePath = tmpPath;
            }
        } else if (*outputPath != '/') {
            exePath = tempSprintf("./%s", outputPath);
        }

        compileProgram(c, exePath);

        const char **args = calloc(argc + 2, sizeof(*args));
        assert(args);

        args[0] = exePath;
        for (int i = 0; i < argc; i++) {
            args[i + 1] = argv[i];
        }

        const int result = runCommand(args);

        // If the user provided an output path along with the run flag, assume that the user needs
        // the executable to persist.
        if (!outputPath) {
            removeFile(exePath);
        }

        return result;
    }

    if (!outputPath) {
        outputPath = tempStrToCstr(strStripSuffix(strFromCstr(inputPath), strFromCstr(".glos")));
    }

    compileProgram(c, outputPath);
    return 0;
}
