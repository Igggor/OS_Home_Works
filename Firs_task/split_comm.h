#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_LINE 1024


int split_command(const char *cmd, char **args, int max_args) {
    if (!cmd) return -1;

    int argc = 0;
    const char *p = cmd;

    while (*p) {
        // Пропускаем пробелы
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        if (argc >= max_args - 1) {
            fprintf(stderr, "Too many arguments (max %d)\n", max_args - 1);
            return -1;
        }

        char token[MAX_LINE];
        int ti = 0;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p+1)) { p++; token[ti++] = *p++; }
                else token[ti++] = *p++;
                if (ti >= (int)sizeof(token)-1) break;
            }
            if (*p == quote) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
                if (*p == '\\' && *(p+1)) { p++; token[ti++] = *p++; }
                else token[ti++] = *p++;
                if (ti >= (int)sizeof(token)-1) break;
            }
        }

        token[ti] = '\0';
        args[argc] = strdup(token);
        if (!args[argc]) {
            perror("strdup");
            for (int i = 0; i < argc; ++i) free(args[i]);
            return -1;
        }
        argc++;
    }

    args[argc] = NULL;
    return argc;
}