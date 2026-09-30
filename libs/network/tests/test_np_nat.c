/*
 * test_np_nat - np_psnr's NAT traversal: probing psnr from the P2P socket,
 * consuming psnr's packets, and punching toward peers. The socket is a fake
 * sender that records what goes out.
 *
 *   clang-cl /I include /Fe:test_np_nat.exe libs/network/tests/test_np_nat.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_nat libs/network/tests/test_np_nat.c
 */
#include "../psnr/psnr.c"
#include "../np_psnr.c"

#include <assert.h>
#ifdef _WIN32
#  define sleep_ms(n) Sleep(n)
#else
#  include <unistd.h>
#  define sleep_ms(n) usleep((n) * 1000)
#endif

const char* np_fake_username(void) { return "PS3Player"; }

static struct { uint8_t pkt[32]; uint32_t len; uint8_t ip[4]; uint16_t port; } s_sent[64];
static int s_nsent;

static int fake_send(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    memcpy(s_sent[s_nsent].pkt, buf, len);
    s_sent[s_nsent].len = len;
    memcpy(s_sent[s_nsent].ip, ip, 4);
    s_sent[s_nsent].port = port;
    s_nsent++;
    return (int)len;
}

int main(void)
{
    /* A connected client, as psnr_connect would leave it. */
    psnr_client* c = (psnr_client*)calloc(1, sizeof(psnr_client));
    c->user_id = 7;
    c->token = 0x1234567;
    memcpy(c->server_ip, "\x0a\x63\x00\x0a", 4);   /* 10.99.0.10 */
    c->server_port = 36100;
    memcpy(c->local_ip, "\xc0\xa8\x0a\x0a", 4);    /* 192.168.10.10 */
    s_client = c;
    s_user_id = 7;

    /* Nothing goes out until the title binds its P2P socket. */
    nat_tick();
    assert(s_nsent == 0);

    /* Then a probe to psnr's UDP port, carrying the token. */
    np_psnr_set_p2p_sender(fake_send);
    nat_tick();
    assert(s_nsent == 1 && s_sent[0].len == PSNR_UDP_PROBE_LEN && s_sent[0].port == 36100);
    assert(!memcmp(s_sent[0].ip, c->server_ip, 4) && !memcmp(s_sent[0].pkt, "PSNR\x01", 5));
    assert(psnr_get32(s_sent[0].pkt + 5) == 7 && psnr_get32(s_sent[0].pkt + 9) == 0x1234567);

    /* Unanswered, it repeats every half second. */
    nat_tick();
    assert(s_nsent == 1);
    sleep_ms(PROBE_MS + 50);
    nat_tick();
    assert(s_nsent == 2);

    /* The reply is psnr's: consumed, and probing drops to a refresh. */
    uint8_t reply[11] = { 'P', 'S', 'N', 'R', 0x81, 203, 0, 113, 7, 0x9C, 0x41 };
    assert(np_psnr_p2p_filter(reply, sizeof(reply)) == 1 && s_probed);
    assert(np_psnr_p2p_filter("PSNR\x02....", 9) == 1);   /* a peer's punch */
    assert(np_psnr_p2p_filter("game data", 9) == 0);     /* the title's own */
    sleep_ms(PROBE_MS + 50);
    nat_tick();
    assert(s_nsent == 2);

    /* A peer appears: five punches, 200 ms apart, to its endpoint. Our own
     * member entry is skipped. */
    const uint8_t peer[4] = { 198, 51, 100, 9 };
    np_psnr_punch(7, peer, 3658);
    np_psnr_punch(9, peer, 40001);
    for (int i = 0; i < 12; i++) { nat_tick(); sleep_ms(PUNCH_MS / 2 + 10); }
    int punches = 0;
    for (int i = 2; i < s_nsent; i++) {
        assert(s_sent[i].len == PSNR_UDP_PUNCH_LEN && !memcmp(s_sent[i].pkt, "PSNR\x02", 5));
        assert(!memcmp(s_sent[i].ip, peer, 4) && s_sent[i].port == 40001);
        punches++;
    }
    assert(punches == PUNCHES);

    /* The socket closes: nothing more goes out. */
    np_psnr_set_p2p_sender(NULL);
    np_psnr_punch(9, peer, 40002);
    sleep_ms(PUNCH_MS + 10);
    nat_tick();
    assert(s_nsent == 2 + PUNCHES);

    printf("test_np_nat: all passed\n");
    return 0;
}
