#include "node.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int all_received(const Node *node, const int *received) {
    int peer;
    for (peer = 1; peer <= node->children; ++peer)
        if (peer != node->id && !received[peer]) return 0;
    return 1;
}

/* Service requests during BOTH barriers, including after our own DONE.
 * Otherwise a short-lived worker can strand a longer-lived one. */
void dispatch(Node *node) {
    Message message, reply;
    int peer;
    await_any(node, &message);
    peer = node->sender;
    switch (message.s_header.s_type) {
    case STARTED:
        node->started[peer] = 1;
        break;
    case DONE:
        node->done[peer] = 1;
        /* FIFO guarantees the peer's final RELEASE was received first. */
        if (node->queued[peer]) fatal("DONE with an outstanding request");
        break;
    case CS_REQUEST:
    case CS_REPLY:
    case CS_RELEASE:
        if (!node->mutex_enabled || peer == PARENT_ID || node->id == PARENT_ID ||
            message.s_header.s_payload_len != 0) fatal("Invalid mutex message");
        if (message.s_header.s_type == CS_REQUEST) {
            if (node->queued[peer] || node->done[peer]) fatal("Duplicate or late request");
            node->queued[peer] = 1;
            node->requested_at[peer] = message.s_header.s_local_time;
            message_init(&reply, CS_REPLY, NULL, 0);
            if (send(node, (local_id)peer, &reply) != 0) fatal("Cannot reply");
        } else if (message.s_header.s_type == CS_REPLY) {
            if (!node->requesting) fatal("Unexpected reply");
            node->replied[peer] = 1;
        } else {
            if (!node->queued[peer]) fatal("Release without request");
            node->queued[peer] = 0;
        }
        break;
    default:
        fatal("Unexpected message type");
    }
}

static void announce(Node *node, MessageType type) {
    Message message;
    char line[256];
    int length;
    /* send_multicast advances the clock once for the whole broadcast. */
    int stamp = get_lamport_time() + 1;
    if (type == STARTED)
        length = snprintf(line, sizeof(line), log_started_fmt,
                          stamp, node->id, (int)getpid(), (int)getppid(), 0);
    else
        length = snprintf(line, sizeof(line), log_done_fmt, stamp, node->id, 0);
    if (length < 0 || (size_t)length >= sizeof(line)) fatal("Announcement too long");
    event(node, "%s", line);
    message_init(&message, type, line, (size_t)length);
    if (send_multicast(node, &message) != 0) fatal("Cannot announce state");
}

void child_run(Node *node) {
    int iteration, total = node->id * 5;
    int diagnostic_fd = dup(STDERR_FILENO);
    if (diagnostic_fd < 0) fatal("Cannot preserve diagnostic stream");
    announce(node, STARTED);
    while (!all_received(node, node->started)) dispatch(node);
    event(node, log_received_all_started_fmt, get_lamport_time(), node->id);
    for (iteration = 1; iteration <= total; ++iteration) {
        char line[128];
        node->iteration = iteration;
        snprintf(line, sizeof(line), log_loop_operation_fmt, node->id, iteration, total);
        if (node->mutex_enabled && request_cs(node) != 0) fatal("Cannot enter CS");
        /* The supplied runtime writes print() to fd 2, one byte at a time.
         * Route only this call to stdout; restore diagnostics immediately. */
        if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) fatal("Cannot route runtime output");
        print(line);
        if (dup2(diagnostic_fd, STDERR_FILENO) < 0) fatal("Cannot restore diagnostics");
        if (node->mutex_enabled && release_cs(node) != 0) fatal("Cannot leave CS");
    }
    announce(node, DONE);
    while (!all_received(node, node->done)) dispatch(node);
    event(node, log_received_all_done_fmt, get_lamport_time(), node->id);
    close(diagnostic_fd);
}

void parent_run(Node *node) {
    while (!all_received(node, node->started)) dispatch(node);
    event(node, log_received_all_started_fmt, get_lamport_time(), node->id);
    while (!all_received(node, node->done)) dispatch(node);
    event(node, log_received_all_done_fmt, get_lamport_time(), node->id);
}
