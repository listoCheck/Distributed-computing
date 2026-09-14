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
    fprintf(stderr, "pa4: process %d: %s\n", process.id, reason);
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

void trace_event(Node *node, const char *action) {
    char line[128];
    int length;
    if (node->trace_fd < 0) return;
    length = snprintf(line, sizeof(line), "%s %d %d %d\n", action,
                      node->id, get_lamport_time(), node->iteration);
    if (length < 0 || (size_t)length >= sizeof(line)) fatal("Trace line too long");
    write_text(node->trace_fd, line, (size_t)length);
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
    int trace_enabled = 0;
    int stats_enabled = 0;
    process.trace_fd = -1;
    process.stats_fd = -1;
    for (id = 1; id < argc; ++id) {
        if (strcmp(argv[id], "-p") == 0 && id + 1 < argc && !process.children) {
            process.children = (local_id)number(argv[++id], 1, MAX_PROCESS_ID);
        } else if (strcmp(argv[id], "--mutexl") == 0 && !process.mutex_enabled) {
            process.mutex_enabled = 1;
        } else if (strcmp(argv[id], "--trace") == 0 && !trace_enabled) {
            trace_enabled = 1;
        } else if (strcmp(argv[id], "--stats") == 0 && !stats_enabled) {
            stats_enabled = 1;
        } else {
            fatal("Usage: ./pa4 -p CHILDREN [--mutexl] [--trace] [--stats]");
        }
    }
    if (!process.children) fatal("Expected -p CHILDREN (1..15)");
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fatal("Cannot configure SIGPIPE");
    }
    process.log_fd = open(events_log, O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0644);
    if (process.log_fd < 0 || channels_open(&process) != 0) {
        channels_close(&process);
        fatal("Cannot create logs or pipes");
    }
    if (trace_enabled) {
        process.trace_fd = open("mutex.trace", O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0644);
        if (process.trace_fd < 0) fatal("Cannot open trace");
    }
    if (stats_enabled) stats_open(&process);
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
            stats_finish(&process);
            channels_close(&process);
            close(process.log_fd);
            if (process.trace_fd >= 0) close(process.trace_fd);
            fflush(stdout);
            _exit(EXIT_SUCCESS);
        }
        process.workers[id] = pid;
    }
    channels_keep_local(&process);
    parent_run(&process);
    stats_finish(&process);
    channels_close(&process);
    close(process.log_fd);
    if (process.trace_fd >= 0) close(process.trace_fd);
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
