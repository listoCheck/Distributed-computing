#define _POSIX_C_SOURCE 200809L
#include "node.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void message_init(Message *message, MessageType type, const void *data, size_t size) {
    if (size > MAX_PAYLOAD_LEN) {
        fatal("Message too large");
    }
    memset(message, 0, sizeof(*message));
    message->s_header.s_magic = MESSAGE_MAGIC;
    message->s_header.s_type = type;
    message->s_header.s_payload_len = (uint16_t)size;
    if (size != 0) {
        memcpy(message->s_payload, data, size);
    }
}

int channels_open(Node *node) {
    int source, target, end;
    FILE *log;
    for (source = 0; source < NODE_COUNT; ++source) {
        for (target = 0; target < NODE_COUNT; ++target) {
            node->channel[source][target][0] = -1;
            node->channel[source][target][1] = -1;
        }
    }
    log = fopen(pipes_log, "w");
    if (log == NULL) {
        return -1;
    }
    for (source = 0; source <= node->children; ++source) {
        for (target = 0; target <= node->children; ++target) {
            int *pair = node->channel[source][target];
            if (source == target) {
                continue;
            }
            if (pipe(pair) != 0) {
                fclose(log);
                return -1;
            }
            for (end = 0; end < 2; ++end) {
                int flags = fcntl(pair[end], F_GETFL);
                if (flags < 0 || fcntl(pair[end], F_SETFL, flags | O_NONBLOCK) < 0) {
                    fclose(log);
                    return -1;
                }
            }
            if (fprintf(log, "%d -> %d: read=%d write=%d\n",
                        source, target, pair[0], pair[1]) < 0) {
                fclose(log);
                return -1;
            }
        }
    }
    return fclose(log) == 0 ? 0 : -1;
}

void channels_keep_local(Node *node) {
    int source, target, end;
    for (source = 0; source <= node->children; ++source) {
        for (target = 0; target <= node->children; ++target) {
            for (end = 0; end < 2; ++end) {
                int *fd = &node->channel[source][target][end];
                int keep = end == 0 ? target == node->id : source == node->id;
                if (*fd >= 0 && !keep) {
                    close(*fd);
                    *fd = -1;
                }
            }
        }
    }
}

void channels_close(Node *node) {
    int source, target, end;
    for (source = 0; source <= node->children; ++source) {
        for (target = 0; target <= node->children; ++target) {
            for (end = 0; end < 2; ++end) {
                int *fd = &node->channel[source][target][end];
                if (*fd >= 0) {
                    close(*fd);
                    *fd = -1;
                }
            }
        }
    }
}

/* This layer never changes the clock; multicast reuses one stamped frame. */
static int write_frame(Node *node, local_id target, const Message *message) {
    size_t offset = 0;
    size_t size = sizeof(MessageHeader) + message->s_header.s_payload_len;
    int fd = node->channel[node->id][target][1];
    while (offset < size) {
        ssize_t count = write(fd, (const char *)message + offset, size - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sched_yield();
        } else {
            return -1;
        }
    }
    return 0;
}

int send(void *self, local_id dst, const Message *msg) {
    Node *node = self;
    Message frame;
    if (dst < 0 || dst > node->children || dst == node->id ||
        msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }
    frame = *msg;
    frame.s_header.s_local_time = clock_send();
    return write_frame(node, dst, &frame);
}

int send_multicast(void *self, const Message *msg) {
    Node *node = self;
    Message frame;
    local_id target;
    if (msg->s_header.s_payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }
    frame = *msg;
    frame.s_header.s_local_time = clock_send();
    for (target = 0; target <= node->children; ++target) {
        if (target != node->id && write_frame(node, target, &frame) != 0) {
            return -1;
        }
    }
    return 0;
}

/* DONE workers keep servicing requests until the final barrier. */
int send_children(Node *node, MessageType type) {
    Message frame;
    local_id peer;
    message_init(&frame, type, NULL, 0);
    frame.s_header.s_local_time = clock_send();
    for (peer = 1; peer <= node->children; ++peer) {
        if (peer != node->id &&
            write_frame(node, peer, &frame) != 0) return -1;
    }
    return 0;
}

/* 0 = complete event, 1 = would block / closed peer, -1 = malformed or I/O error. */
int receive(void *self, local_id from, Message *msg) {
    Node *node = self;
    Decoder *decoder;
    int *fd;
    if (from < 0 || from > node->children || from == node->id) {
        return -1;
    }
    decoder = &node->input[(int)from];
    fd = &node->channel[from][node->id][0];
    if (*fd < 0) {
        return 1;
    }
    for (;;) {
        size_t needed = sizeof(MessageHeader);
        ssize_t count;
        if (decoder->used >= needed) {
            if (decoder->frame.s_header.s_magic != MESSAGE_MAGIC ||
                decoder->frame.s_header.s_payload_len > MAX_PAYLOAD_LEN) {
                return -1;
            }
            needed += decoder->frame.s_header.s_payload_len;
            if (decoder->used == needed) {
                *msg = decoder->frame;
                decoder->used = 0;
                node->sender = from;
                return clock_receive(msg->s_header.s_local_time);
            }
        }
        count = read(*fd, (char *)&decoder->frame + decoder->used,
                     needed - decoder->used);
        if (count > 0) {
            decoder->used += (size_t)count;
        } else if (count == 0) {
            close(*fd);
            *fd = -1;
            return decoder->used == 0 ? 1 : -1;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        } else {
            return -1;
        }
    }
}

int receive_any(void *self, Message *msg) {
    Node *node = self;
    int scanned;
    for (scanned = 0; scanned <= node->children; ++scanned) {
        local_id peer = (local_id)node->cursor;
        int status;
        node->cursor = (node->cursor + 1) % (node->children + 1);
        if (peer == node->id) {
            continue;
        }
        status = receive(node, peer, msg);
        if (status != 1) {
            return status;
        }
    }
    return 1;
}

void await_message(Node *node, local_id from, Message *message) {
    int status;
    while ((status = receive(node, from, message)) == 1) {
        if (node->channel[from][node->id][0] < 0) {
            fatal("Peer closed before expected message");
        }
        sched_yield();
    }
    if (status != 0) {
        fatal("Cannot receive message");
    }
}

void await_any(Node *node, Message *message) {
    int status;
    while ((status = receive_any(node, message)) == 1) {
        int peer, open = 0;
        for (peer = 0; peer <= node->children; ++peer) {
            open += node->channel[peer][node->id][0] >= 0;
        }
        if (!open) {
            fatal("All peers closed before protocol completion");
        }
        sched_yield();
    }
    if (status != 0) {
        fatal("Cannot receive message");
    }
}
