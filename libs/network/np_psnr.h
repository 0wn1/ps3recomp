/*
 * ps3recomp - the connection to a psnr server (libs/network/psnr/)
 *
 * Online NP needs three things set:
 *   PS3_NET_ONLINE=1         real sockets and a connected cellNetCtl
 *   PSNR_SERVER=host[:port]  the psnr server (port 36100 by default)
 *   PS3_NP_ONLINE_ID=name    optional; otherwise the fake NP username
 * and optionally PS3_NET_P2P_PORT (default 3658), the host port this
 * instance's P2P sockets bind. That port goes to peers through psnr; two
 * instances on one machine need different values.
 *
 * One connection per process, made when the first NP context starts.
 * Requests take a reply handler. np_psnr_pump(), called from
 * cellSysutilCheckCallback, runs reply and push handlers on the title's own
 * polling thread. That is where they queue guest callbacks, and the same
 * CheckCallback call delivers them.
 */
#ifndef PS3RECOMP_NP_PSNR_H
#define PS3RECOMP_NP_PSNR_H

#include "psnr/psnr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PS3_NET_ONLINE and PSNR_SERVER are both set. */
int np_psnr_enabled(void);

/* Connect (once) as this title. 0 if connected, -1 if not. */
int np_psnr_connect(const char* comm_id);
int np_psnr_connected(void);

/* The player's online ID, and the P2P port this instance binds and advertises. */
const char* np_psnr_online_id(void);
uint16_t    np_psnr_p2p_port(void);

/* Send a request; fn(user, reply) runs from np_psnr_pump(). A reply of
 * type 0 means the connection dropped before it came. 0 if not sent. */
typedef void (*np_psnr_reply_fn)(void* user, const psnr_msg* reply);
uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len,
                         np_psnr_reply_fn fn, void* user);

/* Where server pushes go (room events). One handler: Matching2. */
void np_psnr_on_push(void (*fn)(const psnr_msg* push));

/* Called at the start of every np_psnr_pump(), for work that falls due over
 * time rather than on a message (Matching2's deferred signaling). */
void np_psnr_on_tick(void (*fn)(void));

void np_psnr_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_NP_PSNR_H */
