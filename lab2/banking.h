#ifndef BANKING_H
#define BANKING_H

#include "ipc.h"

#define MAX_PROCESS_ID 10
#define MAX_T 255

typedef int16_t balance_t;

typedef struct __attribute__((packed)) {
    local_id s_src;
    local_id s_dst;
    balance_t s_amount;
} TransferOrder;

typedef struct __attribute__((packed)) {
    balance_t s_balance;
    balance_t s_balance_pending_in;
} BalanceState;

typedef struct __attribute__((packed)) {
    local_id s_id;
    uint8_t s_history_len;
    BalanceState s_history[MAX_T];
} BalanceHistory;

typedef struct __attribute__((packed)) {
    uint8_t s_history_len;
    BalanceHistory s_history[MAX_PROCESS_ID + 1];
} AllHistory;

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount);
void bank_robbery(void *parent_data, local_id max_id);
void print_history(const AllHistory *history);
timestamp_t get_physical_time(void);

#endif
