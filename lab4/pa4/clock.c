#include "node.h"
#include <limits.h>

static timestamp_t logical_time = 0;

timestamp_t get_lamport_time(void) {
    return logical_time;
}

timestamp_t clock_send(void) {
    if (logical_time == INT16_MAX) {
        fatal("Lamport clock overflow");
    }
    return ++logical_time;
}

int clock_receive(timestamp_t remote) {
    if (remote < 0 || remote == INT16_MAX || logical_time == INT16_MAX) {
        return -1;
    }
    if (remote > logical_time) {
        logical_time = remote;
    }
    ++logical_time;
    return 0;
}
