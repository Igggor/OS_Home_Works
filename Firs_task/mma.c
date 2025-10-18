#include <stdio.h>      // Для printf, fprintf
#include <stdlib.h>     // Для EXIT_FAILURE, EXIT_SUCCESS
#include <string.h>     // Для strcpy, strlen, strcmp, strerror
#include <unistd.h>     // Для close, execlp, fork, unlink
#include <sys/stat.h>   // Для struct stat, stat, mkdir, chmod
#include <sys/types.h>  // Для типов pid_t и других
#include <dirent.h>     // Для DIR, opendir, readdir, closedir
#include <errno.h>      // Для errno
#include <fcntl.h>      // Для open, O_RDONLY, O_WRONLY и флагов
#include <sys/wait.h>   // Для waitpid, WIFEXITED, WEXITSTATUS
#include <limits.h>     // Для PATH_MAX
#include <utime.h>      // Для utime, struct utimbuf

#define BUF_SIZE 8192   // Размер буфера для чтения/записи файлов

// Функция копирования файла src -> dst через временный файл
int copy_file(const char *src, const char *dst) {
    // Открываем исходный файл на чтение
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        fprintf(stderr, "open('%s'): %s\n", src, strerror(errno));
        return -1;
    }

    // Создаем путь к временной копии dst.tmp
    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst) >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "Path too long for tmp (%s)\n", dst);
        close(in_fd);
        return -1;
    }

    // Создаем временный файл для записи
    int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        fprintf(stderr, "open('%s'): %s\n", tmp_path, strerror(errno));
        close(in_fd);
        return -1;
    }

    // Буфер для копирования
    char buf[BUF_SIZE];
    ssize_t r;
    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t w_total = 0;
        while (w_total < r) {
            // Запись части буфера в файл
            ssize_t w = write(out_fd, buf + w_total, r - w_total);
            if (w < 0) {
                if (errno == EINTR) continue; // Если сигнал прервал write — повторяем
                fprintf(stderr, "write('%s'): %s\n", tmp_path, strerror(errno));
                close(in_fd);
                close(out_fd);
                unlink(tmp_path); // удаляем временный файл
                return -1;
            }
            w_total += w;
        }
    }

    if (r < 0) { // Ошибка чтения
        fprintf(stderr, "read('%s'): %s\n", src, strerror(errno));
        close(in_fd);
        close(out_fd);
        unlink(tmp_path);
        return -1;
    }

    // Закрываем файлы
    if (close(in_fd) < 0)
        fprintf(stderr, "close in_fd: %s\n", strerror(errno));
    if (close(out_fd) < 0)
        fprintf(stderr, "close out_fd: %s\n", strerror(errno));

    // Переименовываем временный файл в конечное имя
    if (rename(tmp_path, dst) < 0) {
        fprintf(stderr, "rename('%s','%s'): %s\n", tmp_path, dst, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

// Функция создания директории (рекурсивно, если промежуточные не существуют)
int ensure_dir_exists(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0; // Уже существует как директория
        fprintf(stderr, "Path exists but is not a directory: %s\n", path);
        return -1;
    }

    // Пытаемся создать директорию
    if (mkdir(path, mode) == 0) return 0;

    if (errno == EEXIST) return 0; // Уже существует
    if (errno == ENOENT) { // Не найден путь — нужно создать промежуточные директории
        char tmp[PATH_MAX];
        size_t len = strlen(path);
        if (len >= sizeof(tmp)) {
            fprintf(stderr, "Path too long: %s\n", path);
            return -1;
        }
        strncpy(tmp, path, sizeof(tmp));
        if (tmp[len-1] == '/') tmp[len-1] = '\0';

        for (char *p = tmp + 1; *p; ++p) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, mode); // Игнорируем ошибки, кроме EEXIST
                *p = '/';
            }
        }
        if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir recursive('%s'): %s\n", tmp, strerror(errno));
            return -1;
        }
        return 0;
    }

    fprintf(stderr, "mkdir('%s'): %s\n", path, strerror(errno));
    return -1;
}

// Рекурсивное резервное копирование папки
void backup_recursive(const char *src_dir, const char *dst_dir) {
    DIR *dir = opendir(src_dir);
    if (!dir) {
        fprintf(stderr, "opendir('%s'): %s\n", src_dir, strerror(errno));
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

        struct stat src_stat;
        if (lstat(src_path, &src_stat) < 0) {
            fprintf(stderr, "stat('%s'): %s\n", src_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(src_stat.st_mode)) {
            ensure_dir_exists(dst_path, 0755);
            backup_recursive(src_path, dst_path); // Рекурсивный вызов для поддиректории
        } else if (S_ISREG(src_stat.st_mode)) {
            char gz_path[PATH_MAX];
            snprintf(gz_path, sizeof(gz_path), "%s.gz", dst_path);

            // Проверяем, нужен ли копирование (если уже существует и новее)
            int need_copy = 1;
            struct stat gz_stat;
            if (stat(gz_path, &gz_stat) == 0) {
                if (difftime(src_stat.st_mtime, gz_stat.st_mtime) <= 0)
                    need_copy = 0;
            }

            if (!need_copy) continue;

            // Убеждаемся, что директория для файла существует
            char dst_dir_copy[PATH_MAX];
            strncpy(dst_dir_copy, dst_path, sizeof(dst_dir_copy));
            char *last_slash = strrchr(dst_dir_copy, '/');
            if (last_slash) {
                *last_slash = '\0';
                ensure_dir_exists(dst_dir_copy, 0755);
            }

            // Копируем файл
            printf("Copying: %s -> %s\n", src_path, dst_path);
            if (copy_file(src_path, dst_path) != 0) {
                fprintf(stderr, "Failed to copy %s -> %s\n", src_path, dst_path);
                continue;
            }

            chmod(dst_path, src_stat.st_mode & 0777); // Восстанавливаем права
            struct utimbuf times;
            times.actime = src_stat.st_atime;
            times.modtime = src_stat.st_mtime;
            utime(dst_path, &times); // Восстанавливаем время

            // Сжимаем файл через gzip
            pid_t pid = fork();
            if (pid == 0) {
                execlp("gzip", "gzip", "-f", dst_path, (char *)NULL);
                _exit(127); // Если execlp не сработал
            } else {
                int status;
                waitpid(pid, &status, 0); // Ждем завершения gzip
            }
        }
    }

    closedir(dir);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source_dir> <dest_dir>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    struct stat st;
    if (stat(src, &st) < 0) {
        fprintf(stderr, "Source '%s' not found: %s\n", src, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Source '%s' is not a directory\n", src);
        return EXIT_FAILURE;
    }

    ensure_dir_exists(dst, 0755);
    backup_recursive(src, dst);
    return EXIT_SUCCESS;
}
