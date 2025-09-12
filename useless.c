#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_LINE 1024

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
            fprintf(stderr, "Invalid line format: %s", line);
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

            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            perror("execl"); // если не получилось
            exit(1);
        }
        // Родитель идёт читать следующую строку (не ждём ребёнка)
    }

    fclose(file);

    // Ждём завершения всех процессов
    while (wait(NULL) > 0);

    return 0;
}
