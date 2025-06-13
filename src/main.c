#include <unistd.h>

#include "checker.h"
#include "compiler.h"
#include "parser.h"

static void usage(FILE *file) {
    fprintf(file, "Usage:\n");
    fprintf(file, "    glos COMMAND [...]\n\n");
    fprintf(file, "Commands:\n");
    fprintf(file, "    help            Show this message\n");
    fprintf(file, "    run   [FILE]    Run the program\n");
    fprintf(file, "    build [FILE]    Compile the program\n");
}

static const char *shift(int *argc, char ***argv, const char *expected) {
    if (*argc <= 0) {
        fprintf(stderr, "ERROR: %s not provided\n\n", expected);
        usage(stderr);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

int main(int argc, char **argv) {
    shift(&argc, &argv, "Program name");

    bool        run = false;
    const char *command = shift(&argc, &argv, "Command");
    if (!strcmp(command, "help")) {
        usage(stdout);
        exit(0);
    } else if (!strcmp(command, "run")) {
        run = true;
    } else if (!strcmp(command, "build")) {
        // Pass
    } else {
        fprintf(stderr, "ERROR: Invalid command '%s'\n\n", command);
        usage(stderr);
        exit(1);
    }

    const char *input = shift(&argc, &argv, "Input file");

    Lexer l = {0};
    if (!lexer_open(&l, input)) {
        fprintf(stderr, "ERROR: Could not read file '%s'\n", input);
        exit(1);
    }

    Arena  arena = {0};
    Parser p = {.arena = &arena};
    parse_file(&p, l);

    Context c = {0};
    check_nodes(&c, p.nodes);

    if (run) {
        static char output[] = "/tmp/glos_run_XXXXXX";

        const int fd = mkstemp(output);
        if (fd < 0) {
            fprintf(stderr, "ERROR: Could not create temporary executable\n");
            exit(1);
        } else {
            close(fd);
            remove(output); // TODO: The production compiler need not do this
        }
        compile_nodes(&c, output);

        Cmd cmd = {0};
        da_push(&cmd, output);
        da_push_many(&cmd, argv, argc);
        const int code = cmd_run(&cmd);
        remove(output);
        return code;
    }

    const char *output = temp_sv_to_cstr(sv_strip_suffix(sv_from_cstr(input), sv_from_cstr(".glos")));
    compile_nodes(&c, output);
    return 0;
}
