#ifndef PA4_NODE_H
#define PA4_NODE_H

#include "pa2345.h"
#include <sys/types.h>

enum { NODE_COUNT = MAX_PROCESS_ID + 1 };
typedef struct { Message frame; size_t used; } Decoder;
typedef struct {
    unsigned long sent[3], received[3];
    unsigned long entries, wait_ticks, longest_wait;
} Statistics;
typedef struct {
    local_id id, children, sender;
    int cursor;
    int channel[NODE_COUNT][NODE_COUNT][2];
    Decoder input[NODE_COUNT];
    pid_t workers[NODE_COUNT];
    int log_fd, trace_fd, stats_fd, mutex_enabled;
    Statistics stats;
    int started[NODE_COUNT], done[NODE_COUNT];
    int queued[NODE_COUNT], replied[NODE_COUNT];
    timestamp_t requested_at[NODE_COUNT];
    int requesting, inside, iteration;
} Node;

timestamp_t get_lamport_time(void);
timestamp_t clock_send(void);
int clock_receive(timestamp_t remote);
void message_init(Message *message, MessageType type, const void *data, size_t size);
int channels_open(Node *node);
void channels_keep_local(Node *node);
void channels_close(Node *node);
void await_message(Node *node, local_id from, Message *message);
void await_any(Node *node, Message *message);
int send_children(Node *node, MessageType type);
void fatal(const char *reason);
void event(Node *node, const char *format, ...);
void trace_event(Node *node, const char *action);
void stats_open(Node *node);
void stats_message(Node *node, int type, int outgoing);
void stats_entry(Node *node);
void stats_finish(Node *node);
void dispatch(Node *node);
void child_run(Node *node);
void parent_run(Node *node);

#endif
