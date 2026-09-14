#include "common.h"
#include "ipc.h"
#include "pa1.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int read_fd;
    int write_fd;
} Channel;

typedef struct {
    local_id id;
    int process_count;
    Channel channels[MAX_PROCESS_ID + 1][MAX_PROCESS_ID + 1];
    FILE *events;
} Process;

static FILE *pipes_file;

static int write_all(int fd, const void *buffer, size_t length) {
    const char *cursor = buffer;

    while (length != 0U) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        cursor += written;
        length -= (size_t) written;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length) {
    char *cursor = buffer;

    while (length != 0U) {
        ssize_t received = read(fd, cursor, length);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return -1;
        }
        cursor += received;
        length -= (size_t) received;
    }
    return 0;
}

int send(void *self, local_id dst, const Message *msg) {
    Process *process = self;
    size_t size;

    if (process == NULL || msg == NULL || dst < 0 || dst >= process->process_count ||
        dst == process->id || msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }
    size = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    return write_all(process->channels[process->id][dst].write_fd, msg, size);
}

int send_multicast(void *self, const Message *msg) {
    Process *process = self;
    local_id destination;

    if (process == NULL) {
        return -1;
    }
    for (destination = 0; destination < process->process_count; ++destination) {
        if (destination != process->id && send(process, destination, msg) != 0) {
            return -1;
        }
    }
    return 0;
}

int receive(void *self, local_id from, Message *msg) {
    Process *process = self;

    if (process == NULL || msg == NULL || from < 0 || from >= process->process_count ||
        from == process->id) {
        return -1;
    }
    if (read_all(process->channels[from][process->id].read_fd, &msg->s_header,
                 sizeof(MessageHeader)) != 0 ||
        msg->s_header.s_magic != MESSAGE_MAGIC ||
        msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }
    if (msg->s_header.s_payload_len == 0U) {
        return 0;
    }
    return read_all(process->channels[from][process->id].read_fd, msg->s_payload,
                    msg->s_header.s_payload_len);
}

int receive_any(void *self, Message *msg) {
    Process *process = self;
    local_id source;

    if (process == NULL || msg == NULL) {
        return -1;
    }
    /* Every PA1 participant broadcasts before it waits, so any incoming pipe is safe. */
    for (source = 0; source < process->process_count; ++source) {
        if (source != process->id) {
            return receive(process, source, msg);
        }
    }
    return -1;
}

static void log_event(Process *process, const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stdout, format, arguments);
    va_end(arguments);
    fflush(stdout);

    va_start(arguments, format);
    vfprintf(process->events, format, arguments);
    va_end(arguments);
    fflush(process->events);
}

static void close_unused_descriptors(Process *process) {
    local_id source;
    local_id destination;

    for (source = 0; source < process->process_count; ++source) {
        for (destination = 0; destination < process->process_count; ++destination) {
            Channel *channel = &process->channels[source][destination];

            if (source == destination) {
                continue;
            }
            if (source != process->id) {
                close(channel->write_fd);
                channel->write_fd = -1;
            }
            if (destination != process->id) {
                close(channel->read_fd);
                channel->read_fd = -1;
            }
        }
    }
}

static void close_remaining_descriptors(Process *process) {
    local_id source;
    local_id destination;

    for (source = 0; source < process->process_count; ++source) {
        for (destination = 0; destination < process->process_count; ++destination) {
            Channel *channel = &process->channels[source][destination];

            if (source == destination) {
                continue;
            }
            if (channel->read_fd >= 0) {
                close(channel->read_fd);
            }
            if (channel->write_fd >= 0) {
                close(channel->write_fd);
            }
        }
    }
}

static int make_message(Message *message, MessageType type, const char *text) {
    size_t length = strlen(text);

    if (length > MAX_PAYLOAD_LEN) {
        return -1;
    }
    message->s_header.s_magic = MESSAGE_MAGIC;
    message->s_header.s_payload_len = (uint16_t) length;
    message->s_header.s_type = (int16_t) type;
    message->s_header.s_local_time = 0;
    memcpy(message->s_payload, text, length);
    return 0;
}

static int wait_for_type(Process *process, MessageType expected) {
    bool received[MAX_PROCESS_ID + 1] = { false };
    int remaining = process->process_count - 2;
    local_id source;
    Message message;

    while (remaining > 0) {
        for (source = 1; source < process->process_count; ++source) {
            if (source == process->id || received[(int) source]) {
                continue;
            }
            if (receive(process, source, &message) != 0) {
                return -1;
            }
            if (message.s_header.s_type == expected) {
                received[(int) source] = true;
                --remaining;
            }
        }
    }
    return 0;
}

static int child_work(Process *process) {
    char text[MAX_PAYLOAD_LEN + 1];
    Message message;
    int length;

    length = snprintf(text, sizeof(text), log_started_fmt, process->id, getpid(), getppid());
    if (length < 0 || (size_t) length >= sizeof(text) || make_message(&message, STARTED, text) != 0) {
        return -1;
    }
    log_event(process, "%s", text);
    if (send_multicast(process, &message) != 0 || wait_for_type(process, STARTED) != 0) {
        return -1;
    }
    log_event(process, log_received_all_started_fmt, process->id);

    length = snprintf(text, sizeof(text), log_done_fmt, process->id);
    if (length < 0 || (size_t) length >= sizeof(text) || make_message(&message, DONE, text) != 0) {
        return -1;
    }
    log_event(process, "%s", text);
    if (send_multicast(process, &message) != 0 || wait_for_type(process, DONE) != 0) {
        return -1;
    }
    log_event(process, log_received_all_done_fmt, process->id);
    return 0;
}

static int parent_work(Process *process) {
    Message message;
    local_id source;

    for (source = 1; source < process->process_count; ++source) {
        if (receive(process, source, &message) != 0 || message.s_header.s_type != STARTED) {
            return -1;
        }
    }
    for (source = 1; source < process->process_count; ++source) {
        if (receive(process, source, &message) != 0 || message.s_header.s_type != DONE) {
            return -1;
        }
    }
    return 0;
}

static int parse_process_count(int argc, char *argv[], int *children) {
    char *end;
    long value;

    if (argc != 3 || strcmp(argv[1], "-p") != 0) {
        return -1;
    }
    errno = 0;
    value = strtol(argv[2], &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > MAX_PROCESS_ID) {
        return -1;
    }
    *children = (int) value;
    return 0;
}

int main(int argc, char *argv[]) {
    Process process;
    int children;
    local_id source;
    local_id destination;
    int status = 0;

    if (parse_process_count(argc, argv, &children) != 0) {
        return 1;
    }
    memset(&process, 0, sizeof(process));
    process.id = PARENT_ID;
    process.process_count = children + 1;
    process.events = fopen(events_log, "w");
    pipes_file = fopen(pipes_log, "w");
    if (process.events == NULL || pipes_file == NULL) {
        return 1;
    }
    for (source = 0; source < process.process_count; ++source) {
        for (destination = 0; destination < process.process_count; ++destination) {
            int descriptors[2];

            process.channels[source][destination].read_fd = -1;
            process.channels[source][destination].write_fd = -1;
            if (source == destination) {
                continue;
            }
            if (pipe(descriptors) != 0) {
                return 1;
            }
            process.channels[source][destination].read_fd = descriptors[0];
            process.channels[source][destination].write_fd = descriptors[1];
            fprintf(pipes_file, "%d -> %d: read=%d write=%d\n", source, destination,
                    descriptors[0], descriptors[1]);
        }
    }
    fflush(pipes_file);

    for (destination = 1; destination < process.process_count; ++destination) {
        pid_t pid = fork();

        if (pid < 0) {
            status = 1;
            break;
        }
        if (pid == 0) {
            process.id = destination;
            close_unused_descriptors(&process);
            status = child_work(&process) == 0 ? 0 : 1;
            close_remaining_descriptors(&process);
            fclose(process.events);
            fclose(pipes_file);
            return status;
        }
    }
    close_unused_descriptors(&process);
    if (status == 0 && parent_work(&process) != 0) {
        status = 1;
    }
    for (source = 1; source < process.process_count; ++source) {
        int child_status;

        if (wait(&child_status) < 0 || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
            status = 1;
        }
    }
    close_remaining_descriptors(&process);
    fclose(process.events);
    fclose(pipes_file);
    return status;
}
