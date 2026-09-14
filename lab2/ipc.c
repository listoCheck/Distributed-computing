#include "process.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int send(void *self, local_id dst, const Message *msg)
{
    Process *p = self;
    size_t offset = 0, length;
    const char *bytes = (const char *)msg;
    if (!p || !msg || dst < 0 || dst > p->count || dst == p->id ||
        p->output[(int)dst] < 0 || msg->s_header.s_magic != MESSAGE_MAGIC ||
        msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) {
        errno = EINVAL;
        return -1;
    }
    length = sizeof(MessageHeader) + msg->s_header.s_payload_len;
    while (offset < length) {
        ssize_t n = write(p->output[(int)dst], bytes + offset, length - offset);
        if (n > 0) {
            offset += (size_t)n;
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                            errno == EWOULDBLOCK)) {
            continue;
        } else {
            if (n == 0) errno = EIO;
            return -1;
        }
    }
    return 0;
}

int send_multicast(void *self, const Message *msg)
{
    Process *p = self;
    int dst;
    if (!p) { errno = EINVAL; return -1; }
    for (dst = 0; dst <= p->count; ++dst)
        if (dst != p->id && send(p, (local_id)dst, msg) != 0) return -1;
    return 0;
}

int receive(void *self, local_id from, Message *msg)
{
    Process *p = self;
    InputBuffer *buffer;
    if (!p || !msg || from < 0 || from > p->count || from == p->id ||
        p->input[(int)from] < 0) {
        errno = EINVAL;
        return -1;
    }
    buffer = &p->incoming[(int)from];
    if (buffer->closed) { errno = EPIPE; return -1; }
    for (;;) {
        size_t required = sizeof(MessageHeader);
        ssize_t n;
        if (buffer->used >= sizeof(MessageHeader)) {
            if (buffer->frame.s_header.s_magic != MESSAGE_MAGIC ||
                buffer->frame.s_header.s_payload_len > MAX_PAYLOAD_LEN) {
                errno = EPROTO;
                return -1;
            }
            required += buffer->frame.s_header.s_payload_len;
        }
        if (buffer->used == required) {
            memcpy(msg, &buffer->frame, required);
            buffer->used = 0;
            p->sender = from;
            return 0;
        }
        n = read(p->input[(int)from], (char *)&buffer->frame + buffer->used,
                 required - buffer->used);
        if (n > 0) {
            buffer->used += (size_t)n;
        } else if (n == 0) {
            buffer->closed = 1;
            errno = buffer->used ? EPROTO : EPIPE;
            return -1;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        } else {
            return -1;
        }
    }
}

int receive_any(void *self, Message *msg)
{
    Process *p = self;
    unsigned i, active = 0, total;
    if (!p || !msg) { errno = EINVAL; return -1; }
    total = (unsigned)p->count + 1;
    for (i = 0; i < total; ++i) {
        unsigned from = (p->next_input + i) % total;
        int result;
        if (from == (unsigned)p->id || p->incoming[from].closed ||
            p->input[from] < 0) continue;
        result = receive(p, (local_id)from, msg);
        if (result == 0) {
            p->next_input = (from + 1) % total;
            return 0;
        }
        if (result < 0) {
            if (errno == EPIPE && p->incoming[from].closed) continue;
            return -1;
        }
        ++active;
    }
    if (!active) { errno = EPIPE; return -1; }
    return 1;
}

void close_channels(Process *p)
{
    int peer;
    for (peer = 0; peer <= p->count; ++peer) {
        if (p->input[peer] >= 0) close(p->input[peer]);
        if (p->output[peer] >= 0) close(p->output[peer]);
        p->input[peer] = p->output[peer] = -1;
    }
}
