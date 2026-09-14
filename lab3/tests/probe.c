#define _POSIX_C_SOURCE 200809L
#include "node.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fatal(const char *reason) {
    fprintf(stderr, "%s\n", reason);
    exit(1);
}

static void put(int fd, const void *data, size_t size) {
    assert(write(fd, data, size) == (ssize_t)size);
}

int main(int argc, char **argv) {
    static Node node;
    Ledger ledger;
    Message message, result, raw;
    timestamp_t before;
    int peer;
    if (argc == 2 && strcmp(argv[1], "overflow") == 0) {
        ledger_init(&ledger, 1, 10);
        ledger_extend(&ledger, 255);
        return 2;
    }
    node.children = 2;
    assert(channels_open(&node) == 0);
    assert(get_lamport_time() == 0);
    message_init(&message, STARTED, "abc", 3);
    assert(send_multicast(&node, &message) == 0);
    assert(get_lamport_time() == 1);
    assert(message.s_header.s_local_time == 0);
    for (peer = 1; peer <= 2; ++peer) {
        assert(read(node.channel[0][peer][0], &raw, sizeof(raw)) ==
               (ssize_t)(sizeof(MessageHeader) + 3));
        assert(raw.s_header.s_local_time == 1);
        assert(memcmp(raw.s_payload, "abc", 3) == 0);
    }
    assert(receive(&node, 1, &result) == 1);
    assert(get_lamport_time() == 1);
    message.s_header.s_local_time = 10;
    put(node.channel[1][0][1], &message, 2);
    assert(receive(&node, 1, &result) == 1);
    assert(get_lamport_time() == 1);
    put(node.channel[1][0][1], (char *)&message + 2, sizeof(MessageHeader) - 2);
    assert(receive(&node, 1, &result) == 1);
    assert(get_lamport_time() == 1);
    put(node.channel[1][0][1], message.s_payload, 1);
    assert(receive(&node, 1, &result) == 1);
    put(node.channel[1][0][1], message.s_payload + 1, 2);
    assert(receive(&node, 1, &result) == 0);
    assert(get_lamport_time() == 11);
    assert(memcmp(result.s_payload, "abc", 3) == 0);
    message_init(&message, ACK, NULL, 0);
    message.s_header.s_local_time = 2;
    put(node.channel[1][0][1], &message, sizeof(MessageHeader));
    put(node.channel[2][0][1], &message, sizeof(MessageHeader));
    assert(receive_any(&node, &result) == 0 && node.sender == 1);
    assert(get_lamport_time() == 12);
    assert(receive_any(&node, &result) == 0 && node.sender == 2);
    assert(get_lamport_time() == 13);
    before = get_lamport_time();
    assert(send(&node, 0, &message) == -1);
    assert(get_lamport_time() == before);
    assert(send(&node, 1, &message) == 0);
    assert(get_lamport_time() == 14);
    assert(read(node.channel[0][1][0], &raw, sizeof(raw)) == (ssize_t)sizeof(MessageHeader));
    assert(raw.s_header.s_local_time == 14);
    /* Reject bad magic, and do not count it as a receive event. */
    message.s_header.s_magic = 0;
    put(node.channel[1][0][1], &message, sizeof(MessageHeader));
    assert(receive(&node, 1, &result) == -1);
    assert(get_lamport_time() == 14);
    channels_close(&node);

    ledger_init(&ledger, 1, 10);
    ledger_credit(&ledger, 3, 9, 4);
    for (peer = 0; peer < 9; ++peer) {
        assert(ledger.wire.s_history[peer].s_balance == 10);
        assert(ledger.wire.s_history[peer].s_balance_pending_in == (peer >= 3 ? 4 : 0));
    }
    assert(ledger.wire.s_history[9].s_balance == 14);
    assert(ledger.wire.s_history[9].s_balance_pending_in == 0);
    ledger_debit(&ledger, 12, 2);
    assert(ledger.wire.s_history[11].s_balance == 14);
    assert(ledger.wire.s_history[12].s_balance == 12);
    ledger_extend(&ledger, 254);
    assert(ledger.wire.s_history_len == 255);
    assert(ledger.wire.s_history[254].s_balance == 12);
    assert(ledger.wire.s_history[254].s_balance_pending_in == 0);
    puts("PASS: clocks, multicast, fragmented frames, fairness, pending interval, history boundary");
    return 0;
}
