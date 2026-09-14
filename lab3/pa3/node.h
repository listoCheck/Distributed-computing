#ifndef PA3_NODE_H
#define PA3_NODE_H

#include "banking.h"
#include <sys/types.h>

enum { NODE_COUNT = MAX_PROCESS_ID + 1 };

/* A stream can end in the middle of a frame: keep one decoder per peer. */
typedef struct {
    Message frame;
    size_t used;
} Decoder;

typedef struct {
    local_id id;
    local_id children;
    local_id sender;
    int cursor;
    int channel[NODE_COUNT][NODE_COUNT][2];
    Decoder input[NODE_COUNT];
    pid_t workers[NODE_COUNT];
    int log_fd;
    balance_t initial[NODE_COUNT];
} Node;

typedef struct {
    BalanceHistory wire;
    unsigned length;
} Ledger;

timestamp_t clock_send(void);
int clock_receive(timestamp_t remote);
void message_init(Message *message, MessageType type, const void *data, size_t size);
int channels_open(Node *node);
void channels_keep_local(Node *node);
void channels_close(Node *node);
void await_message(Node *node, local_id from, Message *message);
void await_any(Node *node, Message *message);
void fatal(const char *reason);
void event(Node *node, const char *format, ...);

void ledger_init(Ledger *ledger, local_id id, balance_t balance);
void ledger_extend(Ledger *ledger, timestamp_t time);
void ledger_debit(Ledger *ledger, timestamp_t time, balance_t amount);
void ledger_credit(Ledger *ledger, timestamp_t sent, timestamp_t received,
                   balance_t amount);
void history_finish(AllHistory *history, const balance_t *initial);
void child_run(Node *node);
void parent_run(Node *node);

#endif
