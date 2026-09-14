#include "banking.h"

/* Local replaceable scenario: one unit visits every account and returns. */
void bank_robbery(void *parent_data, local_id max_id) {
    local_id source;
    if (max_id < 2) {
        return;
    }
    for (source = 1; source <= max_id; ++source) {
        local_id target = source == max_id ? 1 : source + 1;
        transfer(parent_data, source, target, 1);
    }
}
