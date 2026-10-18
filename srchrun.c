//Duck.ai
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_FILE "file.vars"
#define LINE_SIZE 8192

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';

    return s;
}

static int run_command(const char *cmd, const char *filename,
                       const char *pattern)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* In CMD, $1 is the filename and $2 is the regex. */
        execl("/bin/sh", "sh", "-c", cmd, "srchrun",
              filename, pattern, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("waitpid");
        return -1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128;
}

int main(int argc, char **argv)
{
    const char *config_path = "file.vars";

    if (argc == 3 && strcmp(argv[1], "-vars") == 0) {
        config_path = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [-vars /path/to/file.vars]\n", argv[0]);
        return 2;
    }

    FILE *config = fopen(config_path, "r");
    if (!config) {
        perror(config_path);
        return 1;
    }

    char *pattern = NULL;
    char *directory = NULL;
    char *command = NULL;
    char *timeout_text = NULL;
    char line[LINE_SIZE];
    unsigned line_number = 0;

    while (fgets(line, sizeof line, config)) {
        line_number++;
        char *entry = trim(line);

        if (*entry == '\0' || *entry == '#')
            continue;

        char *equals = strchr(entry, '=');
        if (!equals) {
            fprintf(stderr, "%s:%u: expected NAME=value\n",
                    CONFIG_FILE, line_number);
            fclose(config);
            return 1;
        }

        *equals = '\0';
        char *name = trim(entry);
        char *value = trim(equals + 1);

        if (*name == '\0') {
            fprintf(stderr, "%s:%u: empty variable name\n",
                    CONFIG_FILE, line_number);
            fclose(config);
            return 1;
        }

        if (strcmp(name, "REGEX") == 0) {
            free(pattern);
            pattern = strdup(value);
        } else if (strcmp(name, "DIR") == 0) {
            free(directory);
            directory = strdup(value);
        } else if (strcmp(name, "CMD") == 0) {
            free(command);
            command = strdup(value);
        } else if (strcmp(name, "TIME_OUT") == 0) {
            free(timeout_text);
            timeout_text = strdup(value);
        } else if (setenv(name, value, 1) != 0) {
            perror("setenv");
            fclose(config);
            return 1;
        }

        if ((strcmp(name, "REGEX") == 0 && !pattern) ||
            (strcmp(name, "DIR") == 0 && !directory) ||
            (strcmp(name, "CMD") == 0 && !command) ||
            (strcmp(name, "TIME_OUT") == 0 && !timeout_text)) {
            perror("strdup");
            fclose(config);
            return 1;
        }
    }
    fclose(config);

    if (!pattern || !command) {
        fprintf(stderr, "file.vars must define REGEX and CMD\n");
        return 1;
    }
    if (!directory)
        directory = strdup(".");
    if (!directory) {
        perror("strdup");
        return 1;
    }

    /* No TIME_OUT means no expiry limit. */
    double timeout_seconds = 0;
    int has_timeout = timeout_text != NULL;

    if (has_timeout) {
        char *end;
        errno = 0;
        timeout_seconds = strtod(timeout_text, &end);
        if (errno || end == timeout_text || *trim(end) != '\0' ||
            timeout_seconds < 0) {
            fprintf(stderr, "TIME_OUT must be a non-negative number of seconds\n");
            return 1;
        }
    }

    regex_t regex;
    int rc = regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char message[256];
        regerror(rc, &regex, message, sizeof message);
        fprintf(stderr, "Invalid REGEX: %s\n", message);
        return 1;
    }

    DIR *dir = opendir(directory);
    if (!dir) {
        perror(directory);
        regfree(&regex);
        return 1;
    }

    char *newest_name = NULL;
    time_t newest_mtime = 0;
    int have_match = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        if (regexec(&regex, entry->d_name, 0, NULL, 0) != 0)
            continue;

        size_t path_len = strlen(directory) + strlen(entry->d_name) + 2;
        char *path = malloc(path_len);
        if (!path) {
            perror("malloc");
            closedir(dir);
            regfree(&regex);
            free(newest_name);
            return 1;
        }

        snprintf(path, path_len, "%s/%s", directory, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) {
            perror(path);
            free(path);
            continue;
        }

        /* Only consider regular files. */
        if (!S_ISREG(st.st_mode)) {
            free(path);
            continue;
        }

        if (!have_match || st.st_mtime > newest_mtime) {
            char *copy = strdup(path);
            if (!copy) {
                perror("strdup");
                free(path);
                closedir(dir);
                regfree(&regex);
                free(newest_name);
                return 1;
            }
            free(newest_name);
            newest_name = copy;
            newest_mtime = st.st_mtime;
            have_match = 1;
        }

        free(path);
    }

    closedir(dir);
    regfree(&regex);

    if (!have_match) {
        fprintf(stderr, "No matching files found\n");
        free(pattern);
        free(directory);
        free(command);
        free(timeout_text);
        return 1;
    }

    if (has_timeout) {
        double age_seconds = difftime(time(NULL), newest_mtime);
        if (age_seconds > timeout_seconds) {
            fprintf(stderr, "Newest matching file is too old; task cancelled\n");
            free(newest_name);
            free(pattern);
            free(directory);
            free(command);
            free(timeout_text);
            return 2;
        }
    }

    int result = run_command(command, newest_name, pattern);

    free(newest_name);
    free(pattern);
    free(directory);
    free(command);
    free(timeout_text);
    return result == 0 ? 0 : 1;
}
 
