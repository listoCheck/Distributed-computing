#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define MAX_PROCESS_ID 10
#define MAX_PAYLOAD_LEN 65535
#define PARENT_ID ((local_id) 0)

typedef int8_t local_id;
typedef int16_t timestamp_t;

#endif
