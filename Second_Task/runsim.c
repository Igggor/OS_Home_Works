#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s N\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int N = atoi(argv[1]);
    if (N <= 0) {
        fprintf(stderr, "Кол-во команд N, выполняющихся одновременно, должно быть натуральным числом\n");
        exit(EXIT_FAILURE);
    }

    int running = 0;
    char line[MAX_LINE];

    while (1) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            --running;
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        line[strcspn(line, "\n")] = '\0';

        if (strlen(line) == 0)
            continue;

        if (running >= N) {
            fprintf(stderr, "[Error]: too many processes running (%d)\n", running);
            continue;
        }

        char *args[MAX_ARGS];
        int argcnt = 0;

        char *token = strtok(line, " ");
        while (token != NULL && argcnt < MAX_ARGS - 1) {
            args[argcnt++] = token;
            token = strtok(NULL, " ");
        }
        args[argcnt] = NULL;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }
        if (pid == 0) {
            execvp(args[0], args);
            perror("execvp");
            exit(EXIT_FAILURE);
        }

        ++running;
    }

    while (running > 0) {
        if (wait(NULL) > 0)
            --running;
    }

    return 0;
}
