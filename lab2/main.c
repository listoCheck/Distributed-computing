#include "pa2345.h"
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static Process *parent;
static FILE *events;
static FILE *pipe_log_file;

static void keep_process_ends(Process *p, int pipes[][MAX_PROCESS_ID + 1][2]) {
    local_id from, to;
    for (from = 0; from <= p->children; ++from) {
        p->read_fd[(int)from] = -1;
        p->write_fd[(int)from] = -1;
    }
    for (from = 0; from <= p->children; ++from) {
        for (to = 0; to <= p->children; ++to) {
            if (from == to) continue;
            if (from == p->id) {
                p->write_fd[(int)to] = pipes[(int)from][(int)to][1];
            } else {
                close(pipes[(int)from][(int)to][1]);
            }
            if (to == p->id) {
                p->read_fd[(int)from] = pipes[(int)from][(int)to][0];
            } else {
                close(pipes[(int)from][(int)to][0]);
            }
        }
    }
}

static void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

static timestamp_t now(void) {
    timestamp_t value = get_physical_time();
    if (value < 0) return 0;
    return value >= MAX_T ? MAX_T - 1 : value;
}

static void event_line(const char *format, ...) {
    va_list args;
    va_list terminal_args;
    va_start(args, format);
    va_copy(terminal_args, args);
    vfprintf(events, format, args);
    fflush(events);
    vprintf(format, terminal_args);
    fflush(stdout);
    va_end(terminal_args);
    va_end(args);
}

static Message message(MessageType type, const void *payload, size_t length) {
    Message result;
    memset(&result, 0, sizeof(result));
    result.s_header.s_magic = MESSAGE_MAGIC;
    result.s_header.s_type = type;
    result.s_header.s_local_time = now();
    result.s_header.s_payload_len = (uint16_t)length;
    if (length != 0) memcpy(result.s_payload, payload, length);
    return result;
}

static int write_full(int fd, const void *data, size_t size) {
    const char *at = data;
    while (size != 0) {
        ssize_t written = write(fd, at, size);
        if (written > 0) { at += written; size -= (size_t)written; continue; }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return 0;
}

int send(void *self, local_id dst, const Message *msg) {
    Process *p = self;
    size_t size = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    if (dst < 0 || dst > p->children || p->write_fd[(int)dst] < 0) return -1;
    return write_full(p->write_fd[(int)dst], msg, size);
}

int send_multicast(void *self, const Message *msg) {
    Process *p = self;
    local_id id;
    for (id = 0; id <= p->children; ++id)
        if (id != p->id && send(p, id, msg) != 0) return -1;
    return 0;
}

static int read_once(int fd, void *data, size_t size) {
    for (;;) {
        ssize_t got = read(fd, data, size);
        if (got == (ssize_t)size) return 0;
        if (got < 0 && errno == EINTR) continue;
        return -1;
    }
}

int receive(void *self, local_id from, Message *msg) {
    Process *p = self;
    int fd;
    if (from < 0 || from > p->children || from == p->id) return -1;
    fd = p->read_fd[(int)from];
    if (fd < 0 || read_once(fd, &msg->s_header, sizeof(msg->s_header)) != 0) return -1;
    if (msg->s_header.s_magic != MESSAGE_MAGIC || msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) return -1;
    if (msg->s_header.s_payload_len != 0 &&
        read_once(fd, msg->s_payload, msg->s_header.s_payload_len) != 0) return -1;
    return 0;
}

int receive_any(void *self, Message *msg) {
    Process *p = self;
    local_id id;
    for (;;) {
        for (id = 0; id <= p->children; ++id) {
            if (id == p->id || p->read_fd[(int)id] < 0) continue;
            if (receive(p, id, msg) == 0) return 0;
        }
    }
}

static void history_to(Process *p, timestamp_t time) {
    unsigned limit = (unsigned)time;
    unsigned i;
    if (limit >= MAX_T) limit = MAX_T - 1;
    for (i = p->history.s_history_len; i <= limit && i < MAX_T; ++i)
        p->history.s_history[i] = p->history.s_history[p->history.s_history_len - 1];
    if (p->history.s_history_len <= limit) p->history.s_history_len = (uint8_t)(limit + 1);
}

static void set_balance(Process *p, balance_t balance) {
    timestamp_t time = now();
    history_to(p, time);
    p->history.s_history[(unsigned)time].s_balance = balance;
    p->history.s_history[(unsigned)time].s_balance_pending_in = 0;
}

static void mark_pending(Process *p, timestamp_t sent, timestamp_t received, balance_t amount) {
    unsigned t;
    if (sent < 0) sent = 0;
    for (t = (unsigned)sent; t < (unsigned)received && t < p->history.s_history_len; ++t)
        p->history.s_history[t].s_balance_pending_in += amount;
}

static void wait_type(Process *p, MessageType wanted, bool *seen) {
    unsigned count = 0;
    Message msg;
    local_id id;
    for (id = 0; id <= p->children; ++id) if (id != p->id && seen[(int)id]) ++count;
    while (count < (unsigned)p->children) {
        if (receive_any(p, &msg) != 0) continue;
        if (msg.s_header.s_type != wanted) continue;
        for (id = 0; id <= p->children; ++id) {
            if (id == p->id || seen[(int)id]) continue;
            /* A typed receive is unnecessary: each sender has one pipe and phase messages are unique. */
            seen[(int)id] = true;
            ++count;
            break;
        }
    }
}

static void child_transfer(Process *p, const Message *msg) {
    TransferOrder order;
    Message reply;
    timestamp_t received = now();
    if (msg->s_header.s_payload_len != sizeof(order)) return;
    memcpy(&order, msg->s_payload, sizeof(order));
    history_to(p, received);
    if (p->id == order.s_src) {
        balance_t old = p->history.s_history[(unsigned)received].s_balance;
        set_balance(p, old - order.s_amount);
        event_line(log_transfer_out_fmt, now(), p->id, order.s_amount, order.s_dst);
        reply = message(TRANSFER, &order, sizeof(order));
        (void)send(p, order.s_dst, &reply);
    } else if (p->id == order.s_dst) {
        balance_t old = p->history.s_history[(unsigned)received].s_balance;
        mark_pending(p, msg->s_header.s_local_time, received, order.s_amount);
        set_balance(p, old + order.s_amount);
        event_line(log_transfer_in_fmt, now(), p->id, order.s_amount, order.s_src);
        reply = message(ACK, NULL, 0);
        (void)send(p, 0, &reply);
    }
}

static void child_main(Process *p, balance_t balance) {
    bool started[MAX_PROCESS_ID + 1] = { false };
    bool done[MAX_PROCESS_ID + 1] = { false };
    Message msg;
    char text[MAX_PAYLOAD_LEN];
    int text_len;
    unsigned done_count = 0;
    p->history.s_id = p->id;
    p->history.s_history_len = 1;
    p->history.s_history[0].s_balance = balance;
    p->history.s_history[0].s_balance_pending_in = 0;
    text_len = snprintf(text, sizeof(text), log_started_fmt, now(), p->id, balance);
    if (text_len < 0 || (size_t)text_len >= sizeof(text)) _exit(EXIT_FAILURE);
    event_line("%s", text);
    msg = message(STARTED, text, (size_t)text_len);
    if (send_multicast(p, &msg) != 0) _exit(EXIT_FAILURE);
    wait_type(p, STARTED, started);
    for (;;) {
        if (receive_any(p, &msg) != 0) continue;
        if (msg.s_header.s_type == TRANSFER) { child_transfer(p, &msg); continue; }
        if (msg.s_header.s_type == STOP) break;
    }
    text_len = snprintf(text, sizeof(text), log_done_fmt, now(), p->id,
                        p->history.s_history[p->history.s_history_len - 1].s_balance);
    if (text_len < 0 || (size_t)text_len >= sizeof(text)) _exit(EXIT_FAILURE);
    event_line("%s", text);
    msg = message(DONE, text, (size_t)text_len);
    if (send_multicast(p, &msg) != 0) _exit(EXIT_FAILURE);
    while (done_count < (unsigned)(p->children - 1)) {
        if (receive_any(p, &msg) != 0) continue;
        if (msg.s_header.s_type == TRANSFER) { child_transfer(p, &msg); continue; }
        if (msg.s_header.s_type == DONE) ++done_count;
    }
    history_to(p, now());
    msg = message(BALANCE_HISTORY, &p->history, sizeof(p->history));
    (void)send(p, 0, &msg);
    _exit(EXIT_SUCCESS);
}

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount) {
    Process *p = parent_data;
    TransferOrder order;
    Message msg;
    if (src < 1 || dst < 1 || src > p->children || dst > p->children || src == dst) return;
    order.s_src = src; order.s_dst = dst; order.s_amount = amount;
    msg = message(TRANSFER, &order, sizeof(order));
    (void)send(p, src, &msg);
    do { (void)receive_any(p, &msg); } while (msg.s_header.s_type != ACK);
}

static int parse_arguments(int argc, char **argv, balance_t *balances, local_id *children) {
    int count, i;
    if (argc < 4 || strcmp(argv[1], "-p") != 0) return -1;
    count = atoi(argv[2]);
    if (count < 2 || count > MAX_PROCESS_ID || argc != count + 3) return -1;
    for (i = 0; i < count; ++i) {
        char *end;
        long value = strtol(argv[i + 3], &end, 10);
        if (*end != '\0' || value < 1 || value > 99) return -1;
        balances[i + 1] = (balance_t)value;
    }
    *children = (local_id)count;
    return 0;
}

int main(int argc, char **argv) {
    Process root;
    balance_t balances[MAX_PROCESS_ID + 1] = { 0 };
    local_id children, from, to, id;
    int pipes[MAX_PROCESS_ID + 1][MAX_PROCESS_ID + 1][2];
    pid_t spawned[MAX_PROCESS_ID + 1];
    bool started[MAX_PROCESS_ID + 1] = { false };
    AllHistory all;
    Message msg;
    unsigned received = 0;
    if (parse_arguments(argc, argv, balances, &children) != 0) {
        fprintf(stderr, "Usage: %s -p N balance1 ... balanceN\n", argv[0]);
        return EXIT_FAILURE;
    }
    events = fopen("events.log", "w");
    if (events == NULL) fail("events.log");
    pipe_log_file = fopen("pipes.log", "w");
    if (pipe_log_file == NULL) fail("pipes.log");
    memset(&root, 0, sizeof(root)); root.id = 0; root.children = children;
    for (from = 0; from <= children; ++from) for (to = 0; to <= children; ++to) {
        pipes[(int)from][(int)to][0] = pipes[(int)from][(int)to][1] = -1;
        if (from != to) {
            int flags;
            if (pipe(pipes[(int)from][(int)to]) != 0) fail("pipe");
            flags = fcntl(pipes[(int)from][(int)to][0], F_GETFL);
            if (flags < 0 || fcntl(pipes[(int)from][(int)to][0], F_SETFL, flags | O_NONBLOCK) < 0) fail("fcntl");
            flags = fcntl(pipes[(int)from][(int)to][1], F_GETFL);
            if (flags < 0 || fcntl(pipes[(int)from][(int)to][1], F_SETFL, flags | O_NONBLOCK) < 0) fail("fcntl");
            fprintf(pipe_log_file, "%d -> %d: read %d, write %d\n", from, to,
                    pipes[(int)from][(int)to][0], pipes[(int)from][(int)to][1]);
        }
    }
    for (id = 1; id <= children; ++id) {
        spawned[(int)id] = fork();
        if (spawned[(int)id] < 0) fail("fork");
        if (spawned[(int)id] == 0) {
            root.id = id;
            keep_process_ends(&root, pipes);
            child_main(&root, balances[(int)id]);
        }
    }
    root.id = 0;
    keep_process_ends(&root, pipes);
    parent = &root;
    wait_type(parent, STARTED, started);
    bank_robbery(parent, children);
    msg = message(STOP, NULL, 0);
    (void)send_multicast(parent, &msg);
    memset(&all, 0, sizeof(all)); all.s_history_len = (uint8_t)children;
    while (received < (unsigned)children) {
        if (receive_any(parent, &msg) != 0 || msg.s_header.s_type != BALANCE_HISTORY) continue;
        if (msg.s_header.s_payload_len == sizeof(BalanceHistory)) {
            BalanceHistory history;
            memcpy(&history, msg.s_payload, sizeof(history));
            all.s_history[(int)history.s_id] = history;
            ++received;
        }
    }
    for (id = 1; id <= children; ++id) (void)waitpid(spawned[(int)id], NULL, 0);
    print_history(&all);
    fclose(events);
    fclose(pipe_log_file);
    return EXIT_SUCCESS;
}
