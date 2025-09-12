#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/wait.h>

#define BUF_SIZE 8192

// Функция копирования файла
int copy_file(const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        perror("open src");
        return -1;
    }

    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("open dst");
        close(in_fd);
        return -1;
    }

    char buf[BUF_SIZE];
    ssize_t bytes;
    while ((bytes = read(in_fd, buf, BUF_SIZE)) > 0) {
        if (write(out_fd, buf, bytes) != bytes) {
            perror("write");
            close(in_fd);
            close(out_fd);
            return -1;
        }
    }

    close(in_fd);
    close(out_fd);

    if (bytes < 0) {
        perror("read");
        return -1;
    }

    return 0;
}

// Функция рекурсивного бэкапа
void backup_recursive(const char *src_dir, const char *dst_dir) {
    DIR *dir = opendir(src_dir);
    if (!dir) {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char src_path[PATH_MAX];
        char dst_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);

        struct stat src_stat, dst_stat;
        if (stat(src_path, &src_stat) < 0) {
            perror("stat src");
            continue;
        }

        if (S_ISDIR(src_stat.st_mode)) {
            // Создаём каталог в dst
            if (mkdir(dst_path, 0755) < 0 && errno != EEXIST) {
                perror("mkdir");
                continue;
            }
            backup_recursive(src_path, dst_path);
        } else if (S_ISREG(src_stat.st_mode)) {
            char gz_path[PATH_MAX];
            snprintf(gz_path, sizeof(gz_path), "%s.gz", dst_path);

            int need_copy = 1;
            if (stat(gz_path, &dst_stat) == 0) {
                // проверяем время модификации
                if (difftime(src_stat.st_mtime, dst_stat.st_mtime) <= 0) {
                    need_copy = 0; // файл не изменился
                }
            }

            if (need_copy) {
                printf("Copying and compressing: %s -> %s.gz\n", src_path, dst_path);
                if (copy_file(src_path, dst_path) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execlp("gzip", "gzip", "-f", dst_path, (char *)NULL);
                        perror("execlp gzip");
                        exit(1);
                    } else if (pid > 0) {
                        waitpid(pid, NULL, 0);
                    } else {
                        perror("fork");
                    }
                }
            }
        }
    }

    closedir(dir);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source_dir> <dest_dir>\n", argv[0]);
        return 1;
    }

    struct stat st;
    if (stat(argv[1], &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Source directory '%s' not found or not a directory\n", argv[1]);
        return 1;
    }

    if (stat(argv[2], &st) < 0) {
        if (mkdir(argv[2], 0755) < 0) {
            perror("mkdir dest");
            return 1;
        }
    }

    backup_recursive(argv[1], argv[2]);
    return 0;
}
