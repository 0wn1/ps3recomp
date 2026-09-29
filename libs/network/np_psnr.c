/*
 * ps3recomp - the connection to a psnr server. See np_psnr.h.
 */
#include "np_psnr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int          s_tried;
static struct { uint32_t req; np_psnr_reply_fn fn; void* user; } s_pending[PENDING_MAX];
static void (*s_push_fn)(const psnr_msg*);

int np_psnr_enabled(void)
{
    return getenv("PS3_NET_ONLINE") && getenv("PSNR_SERVER");
}

const char* np_psnr_online_id(void)
{
    const char* id = getenv("PS3_NP_ONLINE_ID");
    return (id && *id) ? id : np_fake_username();
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
    UNLOCK();
    if (s_client)
        printf("[psnr] connected to %s:%u as \"%s\" (%s), user %u, seen from %u.%u.%u.%u, p2p port %u\n",
               host, port, np_psnr_online_id(), comm_id, user_id, ip[0], ip[1], ip[2], ip[3],
               np_psnr_p2p_port());
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

void np_psnr_pump(void)
{
    psnr_msg m;
    if (s_tick_fn) s_tick_fn();
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
