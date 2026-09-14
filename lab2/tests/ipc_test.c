#include "process.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    Process *p = calloc(1, sizeof(*p));
    Message sent, got;
    int channel[2], flags, i;
    pid_t pid;
    assert(p);
    p->count = 1;
    p->id = 0;
    for (i = 0; i < PROCESS_SLOTS; ++i) p->input[i] = p->output[i] = -1;
    assert(pipe(channel) == 0);
    flags = fcntl(channel[0], F_GETFL);
    assert(fcntl(channel[0], F_SETFL, flags | O_NONBLOCK) == 0);
    p->input[1] = channel[0];
    memset(&sent, 0, sizeof(sent));
    sent.s_header.s_magic = MESSAGE_MAGIC;
    sent.s_header.s_type = TRANSFER;
    sent.s_header.s_payload_len = 9;
    memcpy(sent.s_payload, "123456789", 9);
    assert(receive(p, 1, &got) == 1);
    assert(write(channel[1], &sent, 3) == 3);
    assert(receive(p, 1, &got) == 1);
    assert(p->incoming[1].used == 3);
    assert(write(channel[1], (char *)&sent + 3, sizeof(MessageHeader) - 3) ==
           (ssize_t)sizeof(MessageHeader) - 3);
    assert(receive(p, 1, &got) == 1);
    assert(write(channel[1], sent.s_payload, 4) == 4);
    assert(receive_any(p, &got) == 1);
    assert(write(channel[1], sent.s_payload + 4, 5) == 5);
    assert(receive_any(p, &got) == 0);
    assert(p->sender == 1);
    assert(memcmp(&sent, &got, sizeof(MessageHeader) + 9) == 0);
    /* Consecutive maximum-size frames with backpressure. */
    flags = fcntl(channel[1], F_GETFL);
    assert(fcntl(channel[1], F_SETFL, flags | O_NONBLOCK) == 0);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        p->id = 1;
        p->output[0] = channel[1];
        close(channel[0]);
        sent.s_header.s_payload_len = MAX_PAYLOAD_LEN;
        for (i = 0; i < 64; ++i) {
            memset(sent.s_payload, i, MAX_PAYLOAD_LEN);
            assert(send(p, 0, &sent) == 0);
        }
        close(channel[1]);
        free(p);
        _exit(0);
    }
    close(channel[1]);
    for (i = 0; i < 64; ++i) {
        int result, j;
        do { result = receive_any(p, &got); } while (result == 1);
        assert(result == 0 && got.s_header.s_payload_len == MAX_PAYLOAD_LEN);
        for (j = 0; j < MAX_PAYLOAD_LEN; ++j)
            assert((unsigned char)got.s_payload[j] == i);
    }
    {
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    assert(receive(p, 1, &got) == -1 && errno == EPIPE);
    close(channel[0]);
    free(p);
    return 0;
}
