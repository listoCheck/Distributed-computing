#define _POSIX_C_SOURCE 200809L
#include "node.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Each process owns its counters. Only completed observations reach this file;
 * collecting statistics never sends protocol messages or advances the clock. */
static void append_text(int fd, const char *text, size_t size) {
    while (size != 0) {
        ssize_t written = write(fd, text, size);
        if (written > 0) {
            text += written;
            size -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            fatal("Cannot write statistics");
        }
    }
}

void stats_open(Node *node) {
    const char *header = "id,entries,wait_ticks,max_wait_ticks,"
        "request_sent,reply_sent,release_sent,request_received,reply_received,release_received\n";
    node->stats_fd = open("mutex.stats.csv", O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0644);
    if (node->stats_fd < 0) fatal("Cannot open statistics");
    append_text(node->stats_fd, header, strlen(header));
}

void stats_message(Node *node, int type, int outgoing) {
    unsigned long *counts;
    if (node->stats_fd < 0 || type < CS_REQUEST || type > CS_RELEASE) return;
    counts = outgoing ? node->stats.sent : node->stats.received;
    ++counts[type - CS_REQUEST];
}

void stats_entry(Node *node) {
    unsigned long elapsed;
    if (node->stats_fd < 0) return;
    elapsed = (unsigned long)(get_lamport_time() - node->requested_at[(int)node->id]);
    ++node->stats.entries;
    node->stats.wait_ticks += elapsed;
    if (elapsed > node->stats.longest_wait) node->stats.longest_wait = elapsed;
}

void stats_finish(Node *node) {
    if (node->stats_fd < 0) return;
    if (node->id != PARENT_ID) {
        char row[384];
        const Statistics *s = &node->stats;
        int length = snprintf(row, sizeof(row), "%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
            node->id, s->entries, s->wait_ticks, s->longest_wait,
            s->sent[0], s->sent[1], s->sent[2], s->received[0], s->received[1], s->received[2]);
        if (length < 0 || (size_t)length >= sizeof(row)) fatal("Statistics row too long");
        append_text(node->stats_fd, row, (size_t)length);
    }
    close(node->stats_fd);
    node->stats_fd = -1;
}
