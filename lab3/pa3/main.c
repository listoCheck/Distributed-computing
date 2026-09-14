#define _POSIX_C_SOURCE 200809L
#include "node.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static Node process;

/* The original parent owns child cleanup, including an incomplete fork loop. */
static void stop_workers(void) {
    int id;
    if (process.id != PARENT_ID) {
        return;
    }
    for (id = 1; id <= process.children; ++id) {
        if (process.workers[id] > 0) {
            kill(process.workers[id], SIGTERM);
        }
    }
    for (id = 1; id <= process.children; ++id) {
        if (process.workers[id] > 0) {
            while (waitpid(process.workers[id], NULL, 0) < 0 && errno == EINTR) {
            }
        }
    }
}

void fatal(const char *reason) {
    fprintf(stderr, "pa3: process %d: %s\n", process.id, reason);
    stop_workers();
    _exit(EXIT_FAILURE);
}

static void write_text(int fd, const char *text, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, text + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            fatal("Cannot write log");
        }
    }
}

void event(Node *node, const char *format, ...) {
    char line[512];
    int length;
    va_list arguments;
    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        fatal("Log line is too long");
    }
    write_text(node->log_fd, line, (size_t)length);
    write_text(STDOUT_FILENO, line, (size_t)length);
}

static long number(const char *text, long low, long high) {
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < low || value > high) {
        fatal("Invalid numeric argument");
    }
    return value;
}

int main(int argc, char **argv) {
    int id;
    int failed = 0;
    if (argc < 4 || strcmp(argv[1], "-p") != 0) {
        fatal("Usage: ./pa3 -p CHILDREN BALANCE_1 ... BALANCE_N");
    }
    process.children = (local_id)number(argv[2], 1, MAX_PROCESS_ID);
    if (argc != process.children + 3) {
        fatal("Expected exactly one initial balance per child");
    }
    for (id = 1; id <= process.children; ++id) {
        process.initial[id] = (balance_t)number(argv[id + 2], 1, 99);
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fatal("Cannot configure SIGPIPE");
    }
    process.log_fd = open(events_log, O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0644);
    if (process.log_fd < 0 || channels_open(&process) != 0) {
        channels_close(&process);
        fatal("Cannot create logs or pipes");
    }
    fflush(NULL);
    for (id = 1; id <= process.children; ++id) {
        pid_t pid = fork();
        if (pid < 0) {
            channels_close(&process);
            fatal("Cannot fork child");
        }
        if (pid == 0) {
            process.id = (local_id)id;
            channels_keep_local(&process);
            child_run(&process);
            channels_close(&process);
            close(process.log_fd);
            _exit(EXIT_SUCCESS);
        }
        process.workers[id] = pid;
    }
    channels_keep_local(&process);
    parent_run(&process);
    channels_close(&process);
    close(process.log_fd);
    for (id = 1; id <= process.children; ++id) {
        int status;
        pid_t result;
        do {
            result = waitpid(process.workers[id], &status, 0);
        } while (result < 0 && errno == EINTR);
        process.workers[id] = 0;
        if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed = 1;
        }
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
