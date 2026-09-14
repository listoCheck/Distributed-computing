#ifndef PROCESS_H
#define PROCESS_H

#include "banking.h"

typedef struct {
    local_id id;
    local_id children;
    int read_fd[MAX_PROCESS_ID + 1];
    int write_fd[MAX_PROCESS_ID + 1];
    BalanceHistory history;
} Process;

#endif
