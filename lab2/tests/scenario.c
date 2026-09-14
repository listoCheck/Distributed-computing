#include "banking.h"

void bank_robbery(void *data, local_id count)
{
#ifndef EMPTY_SCENARIO
    int round, src;
    if (count < 2) return;
    for (round = 0; round < 3; ++round) {
        for (src = 1; src <= count; ++src) {
            local_id dst = (local_id)(src == count ? 1 : src + 1);
            transfer(data, (local_id)src, dst, 1);
        }
    }
#else
    (void)data;
    (void)count;
#endif
}
