#include "banking.h"

/* A small local scenario.  The bot replaces this file with its own scenario. */
void bank_robbery(void *parent_data, local_id max_id) {
    if (max_id >= 2) {
        transfer(parent_data, 1, 2, 1);
        transfer(parent_data, 2, 1, 1);
    }
}
