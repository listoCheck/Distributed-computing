#include "node.h"
#include <limits.h>
#include <string.h>

static balance_t checked_balance(int value) {
    if (value < INT16_MIN || value > INT16_MAX) {
        fatal("Balance does not fit the protocol");
    }
    return (balance_t)value;
}

void ledger_init(Ledger *ledger, local_id id, balance_t balance) {
    memset(ledger, 0, sizeof(*ledger));
    ledger->wire.s_id = id;
    ledger->wire.s_history[0].s_balance = balance;
    ledger->length = 1;
    ledger->wire.s_history_len = 1;
}

void ledger_extend(Ledger *ledger, timestamp_t time) {
    /* uint8_t length cannot represent 256 states, even though MAX_T is 255. */
    if (time < 0 || time >= UINT8_MAX || time > MAX_T) {
        fatal("History exceeds the representable time range 0..254");
    }
    while (ledger->length <= (unsigned)time) {
        unsigned index = ledger->length;
        BalanceState *state = &ledger->wire.s_history[index];
        state->s_time = (timestamp_t)index;
        state->s_balance = ledger->wire.s_history[index - 1].s_balance;
        state->s_balance_pending_in = 0;
        ++ledger->length;
    }
    ledger->wire.s_history_len = (uint8_t)ledger->length;
}

void ledger_debit(Ledger *ledger, timestamp_t time, balance_t amount) {
    BalanceState *state;
    ledger_extend(ledger, time);
    state = &ledger->wire.s_history[time];
    state->s_balance = checked_balance((int)state->s_balance - amount);
}

void ledger_credit(Ledger *ledger, timestamp_t sent, timestamp_t received,
                   balance_t amount) {
    int time;
    if (sent < 0 || sent >= received || amount < 0) {
        fatal("Invalid transfer interval");
    }
    ledger_extend(ledger, received);
    /* Reconstruct the channel state retrospectively at the receiving account. */
    for (time = sent; time < received; ++time) {
        BalanceState *state = &ledger->wire.s_history[time];
        state->s_balance_pending_in = checked_balance(
            (int)state->s_balance_pending_in + amount);
    }
    ledger->wire.s_history[received].s_balance = checked_balance(
        (int)ledger->wire.s_history[received].s_balance + amount);
}

void history_finish(AllHistory *history, const balance_t *initial) {
    unsigned account, time, length = 0;
    long expected = 0;
    for (account = 0; account < history->s_history_len; ++account) {
        BalanceHistory *item = &history->s_history[account];
        if (item->s_id != (local_id)(account + 1) || item->s_history_len == 0 ||
            item->s_history[0].s_balance != initial[account + 1]) {
            fatal("Invalid account history");
        }
        if (item->s_history_len > length) {
            length = item->s_history_len;
        }
        expected += initial[account + 1];
    }
    for (account = 0; account < history->s_history_len; ++account) {
        BalanceHistory *item = &history->s_history[account];
        unsigned old_length = item->s_history_len;
        if (item->s_history[old_length - 1].s_balance_pending_in != 0) {
            fatal("Transfer still pending at completion");
        }
        for (time = old_length; time < length; ++time) {
            item->s_history[time] = item->s_history[old_length - 1];
            item->s_history[time].s_time = (timestamp_t)time;
        }
        item->s_history_len = (uint8_t)length;
    }
    for (time = 0; time < length; ++time) {
        long total = 0;
        for (account = 0; account < history->s_history_len; ++account) {
            const BalanceState *state = &history->s_history[account].s_history[time];
            if (state->s_time != (timestamp_t)time || state->s_balance_pending_in < 0) {
                fatal("Invalid balance state");
            }
            total += (long)state->s_balance + state->s_balance_pending_in;
        }
        if (total != expected) {
            fatal("Money conservation check failed");
        }
    }
}
