#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include "split_comm.h"


#define MAX_ARGS 256



int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    FILE *file = fopen(argv[1], "r");
    if (!file) {
        perror("fopen");
        return 1;
    }

    char line[MAX_LINE];
    time_t start_time = time(NULL);

    while (fgets(line, sizeof(line), file)) {
        int delay;
        char command[MAX_LINE];

        if (sscanf(line, "%d %[^\n]", &delay, command) != 2) {
            fprintf(stderr, "Invalid line format: %s\n", line);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }

        if (pid == 0) {
            // Дочерний процесс
            time_t now = time(NULL);
            int elapsed = (int)(now - start_time);

            if (delay > elapsed) {
                sleep(delay - elapsed);
            }

            // // execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            // execvp(command, '');
            // perror("execl");

            char *args[MAX_ARGS];
            int nargs = split_command(command, args, MAX_ARGS);
            if (nargs <= 0) {
                fprintf(stderr, "Failed to parse command: '%s'\n", command);
                exit(1);
            }

            execvp(args[0], args);

            fprintf(stderr, "execvp('%s') failed: %s\n", args[0], strerror(errno));

            for (int i = 0; i < nargs; ++i) free(args[i]);

            exit(1);
        }
    }

    fclose(file);

    while (wait(NULL) > 0);

    return 0;
}
