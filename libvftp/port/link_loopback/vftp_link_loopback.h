/**
 * @file vftp_link_loopback.h
 * @brief In-process vftp_link_t straight into a server instance, with a
 *        deterministic impaired channel: loss, duplication, reordering, a
 *        bounded receive queue that overflows like a real one, and a link
 *        that dies after N packets. No OS, no threads, no real time -- a
 *        "timeout" is an empty queue, so whole lossy transfers run in
 *        milliseconds and repeat bit for bit from a seed.
 *
 * Conversation semantics mirror a CSP server task: open() starts one,
 * close() ends it and calls the server's abort() (rule 3 of vftp_server.h).
 */
#ifndef VFTP_LINK_LOOPBACK_H
#define VFTP_LINK_LOOPBACK_H

#include "vftp/vftp_link.h"
#include "vftp/vftp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VFTP_LOOPBACK_QUEUE
/** Server -> client packets the simulated receive queue holds before it overflows. */
#define VFTP_LOOPBACK_QUEUE 96u
#endif

/** @brief Loopback state: the server it feeds, impairments, statistics. */
typedef struct {
    vftp_burst_server_t *burst; /**< exactly one of these two is set */
    vftp_poll_server_t *poll;   /**< exactly one of these two is set */

    /* impairments, per mille */
    uint16_t loss_up;   /**< client -> server */
    uint16_t loss_down; /**< server -> client */
    uint16_t dup_down;  /**< server -> client duplicated */
    bool reorder;       /**< server -> client delivered in random order */
    uint32_t die_after_tx; /**< the n-th client send (1-based) and all after fail, 0 = never */
    uint32_t seed;      /**< pseudo-random sequence: same seed, same losses */
    bool silent_close;  /**< close() does not reach the server (a CSP connection without RDP sends
                         * nothing on close): only an ABORT/DONE ends its session */

    /* state / statistics */
    bool closed; /**< the server ended the conversation (VFTP_SRV_CLOSE): like a closed CSP
                  * connection, nothing more reaches it until the next open() */
    uint32_t tx_count; /**< client sends so far */
    /** Statistics: packets sent / lost each way, server -> client packets lost to queue overflow. @{ */
    uint32_t up_sent, up_lost, down_sent, down_lost, down_overflow;
    /** @} */
    /** Receive queue state. @{ */
    uint32_t q_head, q_count;
    /** @} */
    uint16_t q_len[VFTP_LOOPBACK_QUEUE]; /**< receive queue */
    uint8_t q[VFTP_LOOPBACK_QUEUE][VFTP_CFG_PACKET_MAX]; /**< receive queue */
} vftp_loopback_t;

/**
 * @brief Fills `link`; clears statistics and the queue, keeps the impairment settings and server.
 * @param[out] link client link to fill
 * @param lb loopback state (burst or poll set by the caller)
 * @param mtu_tx largest packet client -> server
 * @param mtu_rx largest packet server -> client
 */
void vftp_loopback_init( vftp_link_t *link, vftp_loopback_t *lb, uint16_t mtu_tx, uint16_t mtu_rx );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_LINK_LOOPBACK_H */
