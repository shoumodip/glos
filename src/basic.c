#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "basic.h"

bool removeFile(const char *path) {
    return remove(path) >= 0;
}

bool runCommand(const char **args) {
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        execvp(*args, (char *const *) args);
        exit(1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }

    return WEXITSTATUS(status) == 0;
}
