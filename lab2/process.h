#ifndef LAB2_PROCESS_H
#define LAB2_PROCESS_H

#include "banking.h"

enum { PROCESS_LIMIT = 10, PROCESS_SLOTS = MAX_PROCESS_ID + 1 };

typedef struct {
    Message frame;
    size_t used;
    int closed;
} InputBuffer;

typedef struct {
    local_id id;
    local_id count;
    local_id sender;
    unsigned next_input;
    int input[PROCESS_SLOTS];
    int output[PROCESS_SLOTS];
    InputBuffer incoming[PROCESS_SLOTS];
} Process;

/* receive/receive_any: 0 = message, 1 = would block, -1 = error/EOF.
 * Incomplete frames stay in incoming[] until the next call. */
void close_channels(Process *process);

#endif
