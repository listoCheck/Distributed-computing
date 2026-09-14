#define main application_main
#include "../main.c"
#undef main
#include <assert.h>

int main(void)
{
    Process *worker = calloc(1, sizeof(*worker));
    Process *driver = calloc(1, sizeof(*driver));
    int toward_child[4][2], from_child[4][2];
    int peer, status, result;
    pid_t pid;
    Message msg;
    TransferOrder order;
    BalanceHistory h;
    assert(worker && driver);
    worker->id = 1;
    worker->count = driver->count = 3;
    for (peer = 0; peer < PROCESS_SLOTS; ++peer) {
        worker->input[peer] = worker->output[peer] = -1;
        driver->input[peer] = driver->output[peer] = -1;
    }
    for (peer = 0; peer <= 3; ++peer) {
        int flags;
        if (peer == 1) continue;
        assert(pipe(toward_child[peer]) == 0 && pipe(from_child[peer]) == 0);
        flags = fcntl(toward_child[peer][0], F_GETFL);
        assert(fcntl(toward_child[peer][0], F_SETFL, flags | O_NONBLOCK) == 0);
        flags = fcntl(from_child[peer][0], F_GETFL);
        assert(fcntl(from_child[peer][0], F_SETFL, flags | O_NONBLOCK) == 0);
    }
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        is_parent = 0;
        event_fd = open("/dev/null", O_WRONLY);
        assert(event_fd >= 0);
        for (peer = 0; peer <= 3; ++peer) {
            if (peer == 1) continue;
            close(toward_child[peer][1]);
            close(from_child[peer][0]);
            worker->input[peer] = toward_child[peer][0];
            worker->output[peer] = from_child[peer][1];
        }
        child_main(worker, 10);
        close_channels(worker);
        _exit(0);
    }
    for (peer = 0; peer <= 3; ++peer) {
        if (peer == 1) continue;
        close(toward_child[peer][0]);
        close(from_child[peer][1]);
    }
    driver->input[1] = from_child[0][0];
    for (peer = 2; peer <= 3; ++peer) {
        driver->id = (local_id)peer;
        driver->output[1] = toward_child[peer][1];
        msg = make_message(STARTED, get_physical_time(), NULL, 0);
        assert(send(driver, 1, &msg) == 0);
    }
    driver->id = 0;
    wait_message(driver, 1, &msg);
    assert(msg.s_header.s_type == STARTED);
    /* Peer 2 completes before STOP arrives.  Its DONE must not be lost. */
    driver->id = 2;
    driver->output[1] = toward_child[2][1];
    msg = make_message(DONE, get_physical_time(), NULL, 0);
    assert(send(driver, 1, &msg) == 0);
    driver->id = 0;
    driver->output[1] = toward_child[0][1];
    msg = make_message(STOP, get_physical_time(), NULL, 0);
    assert(send(driver, 1, &msg) == 0);
    wait_message(driver, 1, &msg);
    assert(msg.s_header.s_type == DONE);
    /* Only after worker's DONE, peer 3 delivers an in-flight transfer. */
    driver->id = 3;
    driver->output[1] = toward_child[3][1];
    order.s_src = 3; order.s_dst = 1; order.s_amount = 7;
    msg = make_message(TRANSFER, get_physical_time(), &order, sizeof(order));
    assert(send(driver, 1, &msg) == 0);
    msg = make_message(DONE, get_physical_time(), NULL, 0);
    assert(send(driver, 1, &msg) == 0);
    driver->id = 0;
    wait_message(driver, 1, &msg);
    assert(msg.s_header.s_type == ACK);
    wait_message(driver, 1, &msg);
    assert(msg.s_header.s_type == BALANCE_HISTORY);
    memset(&h, 0, sizeof(h));
    memcpy(&h, msg.s_payload, msg.s_header.s_payload_len);
    assert(h.s_id == 1 && h.s_history[h.s_history_len - 1].s_balance == 17);
    do { result = (int)waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    assert(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (peer = 0; peer <= 3; ++peer) {
        if (peer == 1) continue;
        close(toward_child[peer][1]);
        close(from_child[peer][0]);
    }
    free(worker);
    free(driver);
    return 0;
}
