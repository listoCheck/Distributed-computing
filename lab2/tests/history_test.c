#define main application_main
#include "../main.c"
#undef main
#include <assert.h>

int main(void)
{
    BalanceHistory h;
    unsigned i;
    memset(&h, 0, sizeof(h));
    h.s_history_len = 1;
    h.s_history[0].s_balance = 10;
    extend_history(&h, 3);
    h.s_history[3].s_balance = 15;
    extend_history(&h, 7);
    assert(h.s_history_len == 8);
    for (i = 0; i < 8; ++i) {
        assert(h.s_history[i].s_time == (timestamp_t)i);
        assert(h.s_history[i].s_balance == (i < 3 ? 10 : 15));
        assert(h.s_history[i].s_balance_pending_in == 0);
    }
    extend_history(&h, 254);
    assert(h.s_history_len == 255);
    assert(h.s_history[254].s_time == 254);
    assert(h.s_history[254].s_balance == 15);
    return 0;
}
