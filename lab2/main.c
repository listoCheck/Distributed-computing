#define _POSIX_C_SOURCE 200809L
#include "process.h"
#include "pa2345.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int event_fd = -1;
static pid_t child_pids[PROCESS_SLOTS];
static int child_count;
static int is_parent = 1;

static void fatal(const char *reason)
{
    int i;
    perror(reason);
    if (is_parent) {
        for (i = 0; i < child_count; ++i)
            if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
        for (i = 0; i < child_count; ++i) {
            if (child_pids[i] > 0)
                while (waitpid(child_pids[i], NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    exit(EXIT_FAILURE);
}

static void ensure(int success, const char *reason)
{
    if (!success) { errno = EPROTO; fatal(reason); }
}

static void write_text(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t n = write(fd, text, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("write log");
        text += n;
        length -= (size_t)n;
    }
}

static void log_event(const char *format, ...)
{
    char line[512];
    int length;
    va_list args;
    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ensure(length >= 0 && (size_t)length < sizeof(line), "log length");
    write_text(event_fd, line, (size_t)length);
    write_text(STDOUT_FILENO, line, (size_t)length);
}

static Message make_message(MessageType type, timestamp_t time,
                            const void *payload, size_t length)
{
    Message msg;
    ensure(length <= MAX_PAYLOAD_LEN, "message length");
    memset(&msg, 0, sizeof(msg));
    msg.s_header.s_magic = MESSAGE_MAGIC;
    msg.s_header.s_type = type;
    msg.s_header.s_local_time = time;
    msg.s_header.s_payload_len = (uint16_t)length;
    if (length) memcpy(msg.s_payload, payload, length);
    return msg;
}

static void wait_message(Process *p, local_id from, Message *msg)
{
    int result;
    do {
        result = from < 0 ? receive_any(p, msg) : receive(p, from, msg);
    } while (result == 1);
    if (result < 0) fatal("receive");
}

static void multicast(Process *p, const Message *msg)
{
    if (send_multicast(p, msg) != 0) fatal("send_multicast");
}

/* Extend a history using the previous balance.  uint8_t length permits
 * 255 entries, so t=255 cannot be represented without wrapping the length. */
static void extend_history(BalanceHistory *h, timestamp_t time)
{
    unsigned t;
    ensure(time >= 0 && time < UINT8_MAX, "history time out of range");
    for (t = h->s_history_len; t <= (unsigned)time; ++t) {
        h->s_history[t] = h->s_history[t - 1];
        h->s_history[t].s_time = (timestamp_t)t;
        h->s_history[t].s_balance_pending_in = 0;
    }
    if ((unsigned)h->s_history_len <= (unsigned)time)
        h->s_history_len = (uint8_t)(time + 1);
}

static void apply_transfer(Process *p, BalanceHistory *h, const Message *msg)
{
    TransferOrder order;
    Message outgoing;
    timestamp_t time = get_physical_time();
    int balance;
    ensure(msg->s_header.s_payload_len == sizeof(order), "transfer payload");
    memcpy(&order, msg->s_payload, sizeof(order));
    ensure(order.s_src > 0 && order.s_src <= p->count &&
           order.s_dst > 0 && order.s_dst <= p->count &&
           order.s_src != order.s_dst && order.s_amount > 0, "transfer order");
    extend_history(h, time);
    balance = h->s_history[h->s_history_len - 1].s_balance;
    if (p->id == order.s_src && p->sender == PARENT_ID) {
        balance -= order.s_amount;
        ensure(balance >= INT16_MIN, "balance underflow");
        h->s_history[time].s_balance = (balance_t)balance;
        log_event(log_transfer_out_fmt, time, p->id, order.s_amount, order.s_dst);
        outgoing = make_message(TRANSFER, time, &order, sizeof(order));
        if (send(p, order.s_dst, &outgoing) != 0) fatal("forward transfer");
    } else {
        ensure(p->id == order.s_dst && p->sender == order.s_src, "transfer sender");
        balance += order.s_amount;
        ensure(balance <= INT16_MAX, "balance overflow");
        h->s_history[time].s_balance = (balance_t)balance;
        log_event(log_transfer_in_fmt, time, p->id, order.s_amount, order.s_src);
        outgoing = make_message(ACK, get_physical_time(), NULL, 0);
        if (send(p, PARENT_ID, &outgoing) != 0) fatal("send ACK");
    }
}

static void child_main(Process *p, balance_t balance)
{
    BalanceHistory history;
    Message msg;
    char line[512];
    timestamp_t time;
    int length, peer, stopped = 0, done_count = 0;
    unsigned char done[PROCESS_SLOTS] = {0};
    memset(&history, 0, sizeof(history));
    history.s_id = p->id;
    history.s_history_len = 1;
    history.s_history[0].s_balance = balance;
    time = get_physical_time();
    length = snprintf(line, sizeof(line), log_started_fmt,
                      time, p->id, (int)getpid(), (int)getppid(), balance);
    ensure(length >= 0 && (size_t)length < sizeof(line), "STARTED length");
    log_event("%s", line);
    msg = make_message(STARTED, time, line, (size_t)length);
    multicast(p, &msg);
    /* Only other children send STARTED.  Parent never participates. */
    for (peer = 1; peer <= p->count; ++peer) {
        if (peer == p->id) continue;
        wait_message(p, (local_id)peer, &msg);
        ensure(msg.s_header.s_type == STARTED, "expected STARTED");
    }
    log_event(log_received_all_started_fmt, get_physical_time(), p->id);
    while (!stopped || done_count < p->count - 1) {
        wait_message(p, -1, &msg);
        switch (msg.s_header.s_type) {
        case TRANSFER:
            apply_transfer(p, &history, &msg);
            break;
        case DONE:
            ensure(p->sender > 0 && !done[(int)p->sender], "duplicate DONE");
            done[(int)p->sender] = 1;
            ++done_count;
            break;
        case STOP:
            ensure(p->sender == PARENT_ID && !stopped, "unexpected STOP");
            stopped = 1;
            time = get_physical_time();
            extend_history(&history, time);
            length = snprintf(line, sizeof(line), log_done_fmt, time, p->id,
                              history.s_history[history.s_history_len - 1].s_balance);
            ensure(length >= 0 && (size_t)length < sizeof(line), "DONE length");
            log_event("%s", line);
            msg = make_message(DONE, time, line, (size_t)length);
            multicast(p, &msg);
            break;
        default:
            ensure(0, "unexpected child message");
        }
    }
    time = get_physical_time();
    log_event(log_received_all_done_fmt, time, p->id);
    extend_history(&history, time);
    msg = make_message(BALANCE_HISTORY, time, &history,
                       offsetof(BalanceHistory, s_history) +
                       history.s_history_len * sizeof(BalanceState));
    if (send(p, PARENT_ID, &msg) != 0) fatal("send history");
}

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount)
{
    Process *p = parent_data;
    TransferOrder order;
    Message msg;
    ensure(p && p->id == PARENT_ID && src > 0 && src <= p->count &&
           dst > 0 && dst <= p->count && src != dst && amount > 0,
           "invalid transfer");
    order.s_src = src;
    order.s_dst = dst;
    order.s_amount = amount;
    msg = make_message(TRANSFER, get_physical_time(), &order, sizeof(order));
    if (send(p, src, &msg) != 0) fatal("send transfer");
    wait_message(p, dst, &msg);
    ensure(msg.s_header.s_type == ACK && msg.s_header.s_payload_len == 0,
           "expected ACK");
}

static void parent_main(Process *p)
{
    AllHistory all;
    Message msg;
    int peer;
    unsigned max_length = 0;
    memset(&all, 0, sizeof(all));
    all.s_history_len = (uint8_t)p->count;
    for (peer = 1; peer <= p->count; ++peer) {
        wait_message(p, (local_id)peer, &msg);
        ensure(msg.s_header.s_type == STARTED, "parent expected STARTED");
    }
    log_event(log_received_all_started_fmt, get_physical_time(), p->id);
    bank_robbery(p, p->count);
    msg = make_message(STOP, get_physical_time(), NULL, 0);
    multicast(p, &msg);
    for (peer = 1; peer <= p->count; ++peer) {
        wait_message(p, (local_id)peer, &msg);
        ensure(msg.s_header.s_type == DONE, "parent expected DONE");
    }
    log_event(log_received_all_done_fmt, get_physical_time(), p->id);
    for (peer = 1; peer <= p->count; ++peer) {
        BalanceHistory *h = &all.s_history[peer - 1];
        size_t header_size = offsetof(BalanceHistory, s_history);
        wait_message(p, (local_id)peer, &msg);
        ensure(msg.s_header.s_type == BALANCE_HISTORY &&
               msg.s_header.s_payload_len >= header_size &&
               msg.s_header.s_payload_len <= sizeof(*h), "history payload");
        memcpy(h, msg.s_payload, msg.s_header.s_payload_len);
        ensure(h->s_id == peer && h->s_history_len > 0 &&
               msg.s_header.s_payload_len ==
               header_size + h->s_history_len * sizeof(BalanceState), "history size");
        if (max_length < h->s_history_len) max_length = h->s_history_len;
    }
    for (peer = 0; peer < p->count; ++peer)
        extend_history(&all.s_history[peer], (timestamp_t)(max_length - 1));
    print_history(&all);
}

static int parse_number(const char *text, int low, int high)
{
    char *end;
    long value;
    if (*text < '0' || *text > '9') return -1;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || *end || value < low || value > high) return -1;
    return (int)value;
}

int main(int argc, char **argv)
{
    Process *p;
    int pipes[PROCESS_SLOTS][PROCESS_SLOTS][2];
    balance_t balances[PROCESS_SLOTS] = {0};
    int count, i, j, end, pipe_log_fd;
    if (argc < 4 || strcmp(argv[1], "-p") ||
        (count = parse_number(argv[2], 1, PROCESS_LIMIT)) < 0 ||
        argc != count + 3) {
        fprintf(stderr, "Usage: %s -p N balance1 ... balanceN\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (i = 1; i <= count; ++i) {
        int value = parse_number(argv[i + 2], 1, 99);
        if (value < 0) { fprintf(stderr, "Balances must be 1..99\n"); return EXIT_FAILURE; }
        balances[i] = (balance_t)value;
    }
    p = calloc(1, sizeof(*p));
    if (!p) fatal("calloc");
    p->count = (local_id)count;
    for (i = 0; i < PROCESS_SLOTS; ++i) p->input[i] = p->output[i] = -1;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) fatal("signal");
    event_fd = open(events_log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (event_fd < 0) fatal("events.log");
    pipe_log_fd = open(pipes_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pipe_log_fd < 0) fatal("pipes.log");
    for (i = 0; i <= count; ++i) {
        for (j = 0; j <= count; ++j) {
            char line[128];
            int length;
            if (i == j) continue;
            if (pipe(pipes[i][j]) != 0) fatal("pipe");
            for (end = 0; end < 2; ++end) {
                int flags = fcntl(pipes[i][j][end], F_GETFL);
                if (flags < 0 || fcntl(pipes[i][j][end], F_SETFL, flags | O_NONBLOCK) < 0)
                    fatal("fcntl");
            }
            length = snprintf(line, sizeof(line), "%d -> %d: read %d, write %d\n",
                              i, j, pipes[i][j][0], pipes[i][j][1]);
            ensure(length > 0 && (size_t)length < sizeof(line), "pipe log");
            write_text(pipe_log_fd, line, (size_t)length);
        }
    }
    close(pipe_log_fd);
    for (i = 1; i <= count; ++i) {
        pid_t pid = fork();
        if (pid < 0) fatal("fork");
        if (pid == 0) {
            is_parent = 0;
            p->id = (local_id)i;
            break;
        }
        child_pids[child_count++] = pid;
    }
    for (i = 0; i <= count; ++i) {
        for (j = 0; j <= count; ++j) {
            if (i == j) continue;
            if (i == p->id) p->output[j] = pipes[i][j][1];
            else close(pipes[i][j][1]);
            if (j == p->id) p->input[i] = pipes[i][j][0];
            else close(pipes[i][j][0]);
        }
    }
    if (p->id == PARENT_ID) parent_main(p);
    else child_main(p, balances[(int)p->id]);
    close_channels(p);
    close(event_fd);
    free(p);
    if (is_parent) {
        for (i = 0; i < child_count; ++i) {
            int status;
            pid_t result;
            do { result = waitpid(child_pids[i], &status, 0); } while (result < 0 && errno == EINTR);
            if (result < 0) fatal("waitpid");
            child_pids[i] = 0;
            ensure(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child failed");
        }
    }
    return EXIT_SUCCESS;
}
