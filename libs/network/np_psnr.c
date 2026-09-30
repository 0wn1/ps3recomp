/*
 * ps3recomp - the connection to a psnr server. See np_psnr.h.
 */
#include "np_psnr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&s_lock)
#  define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* The fake username from sceNp.c. */
extern const char* np_fake_username(void);

#define PENDING_MAX 64

static psnr_client* s_client;
static uint32_t     s_user_id;   /* ours, from HELLO: our own member entry needs no punch */
static int          s_tried;
static struct { uint32_t req; np_psnr_reply_fn fn; void* user; } s_pending[PENDING_MAX];
static void (*s_push_fn)(const psnr_msg*);

static void set_env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* PSN online IDs: 3-16 letters, digits, '-' and '_', starting with a letter.
 * Copies the allowed characters of `in`; 1 if the result is a valid ID. */
static int clean_online_id(const char* in, char out[17])
{
    int n = 0;
    for (; in && *in && n < 16; in++)
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_')
            out[n++] = *in;
    out[n] = 0;
    return n >= 3 && isalpha((unsigned char)out[0]);
}

static char s_id[17];
static int  s_id_state;     /* 0 unresolved, 1 a name applies, 2 none (offline) */

void np_psnr_setup(const char* username, const char* server)
{
    /* Before anything reads them: the runtime's getenv cache keeps the first
     * answer it sees. */
    if (server && *server) {
        set_env("PS3_NET_ONLINE", "1");
        set_env("PSNR_SERVER", server);
    }
    /* The first valid one of: --username, PS3_NP_ONLINE_ID, and -- online
     * only -- the OS login name. */
    const char* named[2] = { username, getenv("PS3_NP_ONLINE_ID") };
    int found = 0;
    for (int i = 0; i < 2 && !found; i++) {
        if (!named[i] || !*named[i]) continue;
        found = clean_online_id(named[i], s_id);
        if (!found)
            printf("[psnr] \"%s\" is not a valid online ID (3-16 letters, digits, - or _, "
                   "starting with a letter)\n", named[i]);
    }
    if (!found) {
        if (!np_psnr_enabled()) { s_id_state = 2; return; }   /* offline: "PS3Player" */
#ifdef _WIN32
        const char* os = getenv("USERNAME");
#else
        const char* os = getenv("USER");
#endif
        if (!clean_online_id(os, s_id)) strcpy(s_id, "PS3Player");
    }
    s_id_state = 1;
    printf("[psnr] player \"%s\"%s\n", s_id, np_psnr_enabled() ? "" : " (offline)");
}

const char* np_psnr_identity(void)
{
    if (!s_id_state) np_psnr_setup(NULL, NULL);
    return s_id_state == 1 ? s_id : NULL;
}

int np_psnr_enabled(void)
{
    return getenv("PS3_NET_ONLINE") && getenv("PSNR_SERVER");
}

const char* np_psnr_online_id(void)
{
    const char* id = np_psnr_identity();
    return id ? id : np_fake_username();
}

uint16_t np_psnr_p2p_port(void)
{
    const char* p = getenv("PS3_NET_P2P_PORT");
    return (uint16_t)(p ? atoi(p) : 3658);
}

int np_psnr_connected(void)
{
    return s_client != NULL;
}

int np_psnr_connect(const char* comm_id)
{
    char host[256];
    uint16_t port = 36100;
    uint32_t user_id = 0;
    uint8_t ip[4] = {0};

    if (!np_psnr_enabled()) return -1;
    LOCK();
    if (s_client || s_tried) {
        int r = s_client ? 0 : -1;
        UNLOCK();
        return r;
    }
    /* ponytail: one attempt per process; retry on the next context start if a
     * server that comes up late turns out to matter. */
    s_tried = 1;
    snprintf(host, sizeof(host), "%s", getenv("PSNR_SERVER"));
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }

    s_client = psnr_connect(host, port, comm_id, np_psnr_online_id(),
                            np_psnr_p2p_port(), &user_id, ip);
    s_user_id = user_id;
    UNLOCK();
    if (s_client)
        printf("[psnr] connected to %s:%u as \"%s\" (%s), user %u, seen from %u.%u.%u.%u, p2p port %u\n",
               host, port, np_psnr_online_id(), comm_id, user_id, ip[0], ip[1], ip[2], ip[3],
               np_psnr_p2p_port());
    else if (psnr_connect_error() == PSNR_E_NAME_TAKEN)
        printf("[psnr] %s:%u: another player there is already \"%s\" -- pick another "
               "with --username. NP stays offline\n", host, port, np_psnr_online_id());
    else
        printf("[psnr] could not reach %s:%u -- NP stays offline\n", host, port);
    return s_client ? 0 : -1;
}

uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len,
                         np_psnr_reply_fn fn, void* user)
{
    uint32_t req = 0;
    LOCK();
    if (s_client) {
        int slot = -1;
        for (int i = 0; i < PENDING_MAX; i++)
            if (!s_pending[i].req) { slot = i; break; }
        if (slot >= 0 && (req = psnr_send(s_client, type, body, len)) != 0) {
            s_pending[slot].req = req;
            s_pending[slot].fn = fn;
            s_pending[slot].user = user;
        }
    }
    UNLOCK();
    return req;
}

void np_psnr_on_push(void (*fn)(const psnr_msg*))
{
    s_push_fn = fn;
}

static void (*s_tick_fn)(void);
void np_psnr_on_tick(void (*fn)(void))
{
    s_tick_fn = fn;
}

/* ---- NAT traversal: probes and punches from the title's P2P socket ---- */

#define PUNCHES      5      /* per peer */
#define PUNCH_MS     200
#define PROBE_MS     500    /* until psnr answers */
#define REPROBE_MS   20000  /* after: routers forget idle UDP mappings */

static np_psnr_p2p_send_fn s_p2p_send;
static int      s_probed;
static uint64_t s_next_probe;
static struct { uint8_t ip[4]; uint16_t port; int left; uint64_t next; } s_punch[8];

static uint64_t nat_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

void np_psnr_set_p2p_sender(np_psnr_p2p_send_fn fn)
{
    s_p2p_send = fn;
    s_probed = 0;
    s_next_probe = 0;
}

int np_psnr_p2p_filter(const void* buf, uint32_t len)
{
    uint8_t ip[4];
    uint16_t port;
    if (!psnr_is_control(buf, len)) return 0;
    if (psnr_probe_reply(buf, len, ip, &port) && !s_probed) {
        s_probed = 1;
        s_next_probe = nat_now_ms() + REPROBE_MS;
        printf("[psnr] P2P socket seen at %u.%u.%u.%u:%u\n", ip[0], ip[1], ip[2], ip[3], port);
    }
    return 1;
}

void np_psnr_punch(uint32_t user, const uint8_t ip[4], uint16_t port)
{
    int slot = -1;
    if (user == s_user_id || !port) return;
    for (int i = 0; i < 8; i++) {
        if (s_punch[i].port == port && !memcmp(s_punch[i].ip, ip, 4)) { slot = i; break; }
        if (slot < 0 && !s_punch[i].left) slot = i;
    }
    if (slot < 0) return;   /* ponytail: 8 peers punching at once; a room has at most 4 */
    memcpy(s_punch[slot].ip, ip, 4);
    s_punch[slot].port = port;
    s_punch[slot].left = PUNCHES;
    s_punch[slot].next = 0;
}

/* From np_psnr_pump: probe psnr until it answers (and again now and then),
 * and send the punches that are due. */
static void nat_tick(void)
{
    uint8_t pkt[PSNR_UDP_PROBE_LEN], ip[4];
    uint16_t port;
    uint64_t now = nat_now_ms();
    if (!s_client || !s_p2p_send) return;
    if (now >= s_next_probe) {
        psnr_probe_packet(s_client, pkt);
        psnr_server_addr(s_client, ip, &port);
        s_p2p_send(pkt, PSNR_UDP_PROBE_LEN, ip, port);
        s_next_probe = now + (s_probed ? REPROBE_MS : PROBE_MS);
    }
    for (int i = 0; i < 8; i++) {
        if (!s_punch[i].left || now < s_punch[i].next) continue;
        psnr_punch_packet(s_client, pkt);
        s_p2p_send(pkt, PSNR_UDP_PUNCH_LEN, s_punch[i].ip, s_punch[i].port);
        s_punch[i].left--;
        s_punch[i].next = now + PUNCH_MS;
    }
}

void np_psnr_pump(void)
{
    psnr_msg m;
    if (s_tick_fn) s_tick_fn();
    nat_tick();
    if (!s_client) return;

    for (;;) {
        np_psnr_reply_fn fn = NULL;
        void* user = NULL;
        LOCK();
        if (!s_client) { UNLOCK(); return; }
        int r = psnr_poll(s_client, &m);
        if (r < 0) {
            /* Gone: fail every outstanding request so no title waits forever. */
            printf("[psnr] connection lost\n");
            psnr_close(s_client);
            s_client = NULL;
            psnr_msg dead;
            memset(&dead, 0, sizeof(dead));
            for (int i = 0; i < PENDING_MAX; i++)
                if (s_pending[i].req) {
                    np_psnr_reply_fn f = s_pending[i].fn;
                    void* u = s_pending[i].user;
                    s_pending[i].req = 0;
                    UNLOCK();
                    if (f) f(u, &dead);
                    LOCK();
                }
            UNLOCK();
            return;
        }
        if (r == 0) { UNLOCK(); return; }
        if (m.req) {
            for (int i = 0; i < PENDING_MAX; i++)
                if (s_pending[i].req == m.req) {
                    fn = s_pending[i].fn;
                    user = s_pending[i].user;
                    s_pending[i].req = 0;
                    break;
                }
        }
        UNLOCK();

        /* Handlers run unlocked: they send follow-up requests. */
        if (m.req && fn) fn(user, &m);
        else if (!m.req && s_push_fn) s_push_fn(&m);
        psnr_msg_free(&m);
    }
}
