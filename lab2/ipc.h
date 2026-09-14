#ifndef IPC_H
#define IPC_H

#include <stdint.h>

#define MESSAGE_MAGIC 0xAFAF
#define MAX_PAYLOAD_LEN 4096

typedef int8_t local_id;
typedef int16_t timestamp_t;

typedef enum {
    STARTED = 1, DONE = 2, STOP = 3, TRANSFER = 4,
    ACK = 5, BALANCE_HISTORY = 6
} MessageType;

typedef struct __attribute__((packed)) {
    uint16_t s_magic;
    uint16_t s_payload_len;
    int16_t s_local_time;
    int16_t s_type;
} MessageHeader;

typedef struct {
    MessageHeader s_header;
    char s_payload[MAX_PAYLOAD_LEN];
} Message;

int send(void *self, local_id dst, const Message *msg);
int send_multicast(void *self, const Message *msg);
int receive(void *self, local_id from, Message *msg);
int receive_any(void *self, Message *msg);

#endif
