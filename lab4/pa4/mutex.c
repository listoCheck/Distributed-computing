#include "node.h"
#include <string.h>

/* One outstanding request per process: a table is enough for a sorted queue.
 * Ordering is lexicographic (Lamport timestamp, local process id). */
static int first_and_confirmed(const Node *node) {
    int peer;
    timestamp_t own = node->requested_at[(int)node->id];
    for (peer = 1; peer <= node->children; ++peer) {
        if (peer == node->id) continue;
        if (!node->replied[peer]) return 0;
        if (node->queued[peer] &&
            (node->requested_at[peer] < own ||
             (node->requested_at[peer] == own && peer < node->id))) return 0;
    }
    return 1;
}

int request_cs(const void *self) {
    Node *node = (Node *)self;
    if (node->id == PARENT_ID || node->requesting || node->inside) return -1;
    memset(node->replied, 0, sizeof(node->replied));
    node->requesting = 1;
    node->queued[(int)node->id] = 1;
    if (send_children(node, CS_REQUEST) != 0) return -1;
    node->requested_at[(int)node->id] = get_lamport_time();
    trace_event(node, "REQUEST");
    while (!first_and_confirmed(node)) dispatch(node);
    node->inside = 1;
    stats_entry(node);
    trace_event(node, "ENTER");
    return 0;
}

int release_cs(const void *self) {
    Node *node = (Node *)self;
    if (!node->inside || !node->requesting) return -1;
    trace_event(node, "EXIT");
    node->inside = 0;
    node->requesting = 0;
    node->queued[(int)node->id] = 0;
    return send_children(node, CS_RELEASE);
}
