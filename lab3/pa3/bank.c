#include "node.h"
#include "pa2345.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void broadcast_status(Node *node, MessageType type, balance_t balance) {
    char line[256];
    Message message;
    int size;
    /* Formatting a log is not an event. send_multicast advances the clock. */
    timestamp_t at = get_lamport_time() + 1;
    if (type == STARTED) {
        size = snprintf(line, sizeof(line), log_started_fmt, at, node->id,
                        (int)getpid(), (int)getppid(), balance);
    } else {
        size = snprintf(line, sizeof(line), log_done_fmt, at, node->id, balance);
    }
    if (size < 0 || (size_t)size >= sizeof(line)) {
        fatal("Cannot format status");
    }
    message_init(&message, type, line, (size_t)size);
    if (send_multicast(node, &message) != 0) {
        fatal("Cannot broadcast status");
    }
    event(node, "%s", line);
}

static void wait_started(Node *node) {
    local_id peer;
    for (peer = 1; peer <= node->children; ++peer) {
        Message message;
        if (peer == node->id) {
            continue;
        }
        await_message(node, peer, &message);
        if (message.s_header.s_type != STARTED) {
            fatal("Expected STARTED");
        }
    }
    event(node, log_received_all_started_fmt, get_lamport_time(), node->id);
}

static void process_transfer(Node *node, Ledger *ledger, const Message *message) {
    TransferOrder order;
    Message outgoing;
    if (message->s_header.s_payload_len != sizeof(order)) {
        fatal("Invalid transfer size");
    }
    memcpy(&order, message->s_payload, sizeof(order));
    if (order.s_src < 1 || order.s_src > node->children ||
        order.s_dst < 1 || order.s_dst > node->children ||
        order.s_src == order.s_dst || order.s_amount < 0) {
        fatal("Invalid transfer order");
    }
    if (node->id == order.s_src && node->sender == PARENT_ID) {
        timestamp_t sent = get_lamport_time() + 1;
        /* The debit belongs to the outgoing TRANSFER event, not the request. */
        ledger_debit(ledger, sent, order.s_amount);
        message_init(&outgoing, TRANSFER, &order, sizeof(order));
        if (send(node, order.s_dst, &outgoing) != 0) {
            fatal("Cannot forward transfer");
        }
        event(node, log_transfer_out_fmt, get_lamport_time(), node->id,
              order.s_amount, order.s_dst);
    } else if (node->id == order.s_dst && node->sender == order.s_src) {
        ledger_credit(ledger, message->s_header.s_local_time,
                      get_lamport_time(), order.s_amount);
        event(node, log_transfer_in_fmt, get_lamport_time(), node->id,
              order.s_amount, order.s_src);
        message_init(&outgoing, ACK, NULL, 0);
        if (send(node, PARENT_ID, &outgoing) != 0) {
            fatal("Cannot acknowledge transfer");
        }
    } else {
        fatal("Transfer arrived on an incorrect channel");
    }
}

void child_run(Node *node) {
    Ledger ledger;
    Message message;
    unsigned done_mask = 0;
    int done_count = 0;
    int stopped = 0;
    size_t size;
    ledger_init(&ledger, node->id, node->initial[(int)node->id]);
    broadcast_status(node, STARTED, node->initial[(int)node->id]);
    wait_started(node);
    while (!stopped || done_count < node->children - 1) {
        await_any(node, &message);
        switch (message.s_header.s_type) {
            case TRANSFER:
                process_transfer(node, &ledger, &message);
                break;
            case STOP:
                if (node->sender != PARENT_ID || stopped ||
                    message.s_header.s_payload_len != 0) {
                    fatal("Unexpected STOP");
                }
                stopped = 1;
                ledger_extend(&ledger, get_lamport_time());
                broadcast_status(node, DONE,
                    ledger.wire.s_history[ledger.length - 1].s_balance);
                break;
            case DONE: {
                unsigned bit = 1u << node->sender;
                if (node->sender == PARENT_ID || (done_mask & bit) != 0) {
                    fatal("Unexpected DONE");
                }
                done_mask |= bit;
                ++done_count;
                break;
            }
            default:
                fatal("Unexpected child message");
        }
    }
    event(node, log_received_all_done_fmt, get_lamport_time(), node->id);
    ledger_extend(&ledger, get_lamport_time());
    size = offsetof(BalanceHistory, s_history) +
           ledger.length * sizeof(BalanceState);
    message_init(&message, BALANCE_HISTORY, &ledger.wire, size);
    if (send(node, PARENT_ID, &message) != 0) {
        fatal("Cannot send balance history");
    }
}

void transfer(void *parent_data, local_id src, local_id dst, balance_t amount) {
    Node *node = parent_data;
    TransferOrder order;
    Message message;
    if (node->id != PARENT_ID || src < 1 || src > node->children ||
        dst < 1 || dst > node->children || src == dst || amount < 0) {
        fatal("Invalid transfer request");
    }
    order.s_src = src;
    order.s_dst = dst;
    order.s_amount = amount;
    message_init(&message, TRANSFER, &order, sizeof(order));
    if (send(node, src, &message) != 0) {
        fatal("Cannot request transfer");
    }
    await_message(node, dst, &message);
    if (message.s_header.s_type != ACK || message.s_header.s_payload_len != 0) {
        fatal("Expected ACK from destination");
    }
}

void parent_run(Node *node) {
    AllHistory history;
    Message message;
    local_id peer;
    memset(&history, 0, sizeof(history));
    history.s_history_len = (uint8_t)node->children;
    wait_started(node);
    bank_robbery(node, node->children);
    message_init(&message, STOP, NULL, 0);
    if (send_multicast(node, &message) != 0) {
        fatal("Cannot broadcast STOP");
    }
    /* FIFO per peer lets early histories stay queued until every DONE arrives. */
    for (peer = 1; peer <= node->children; ++peer) {
        await_message(node, peer, &message);
        if (message.s_header.s_type != DONE) {
            fatal("Expected DONE");
        }
    }
    event(node, log_received_all_done_fmt, get_lamport_time(), node->id);
    for (peer = 1; peer <= node->children; ++peer) {
        BalanceHistory *item = &history.s_history[peer - 1];
        size_t prefix = offsetof(BalanceHistory, s_history);
        size_t expected;
        await_message(node, peer, &message);
        if (message.s_header.s_type != BALANCE_HISTORY ||
            message.s_header.s_payload_len < prefix ||
            message.s_header.s_payload_len > sizeof(*item)) {
            fatal("Invalid history message");
        }
        memcpy(item, message.s_payload, message.s_header.s_payload_len);
        expected = prefix + (size_t)item->s_history_len * sizeof(BalanceState);
        if (item->s_id != peer || item->s_history_len == 0 ||
            expected != message.s_header.s_payload_len) {
            fatal("Invalid history length or owner");
        }
    }
    history_finish(&history, node->initial);
    print_history(&history);
}
