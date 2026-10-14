/**
 * @file vftp_link.h
 * @brief What the client needs from a transport: datagrams in, datagrams out.
 *
 * One vftp packet per datagram, delivered whole or not at all. Loss,
 * duplication and reordering are all survivable (vftp has its own ARQ); a
 * transport that adds reliability (CSP RDP) only makes rounds rarer. Ports in
 * this repository: CSP 1.x (v-client), CSP 2.1 (OBC, server side), CCSDS Space
 * Packets over UDP (port/ccsds_spp), in-process loopback (port/link_loopback).
 */
#ifndef VFTP_LINK_H
#define VFTP_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A datagram transport as the client sees it. How to write one: docs/porting.md.
 */
typedef struct vftp_link {
    /** Optional: begin one conversation (e.g. csp_connect). 0 = ok. Every client call opens
     * once and closes once, whatever the outcome. */
    int ( *open )( void *ctx );
    /** Optional: end the conversation (e.g. csp_close). */
    void ( *close )( void *ctx );
    /** Hand one packet to the transport: 0 = sent, < 0 = the link is broken. May block for
     * flow control. */
    int ( *send )( void *ctx, const uint8_t *pkt, uint16_t len );
    /** Wait for one packet: > 0 = its length, 0 = nothing arrived within timeout_ms, < 0 = link
     * broken. A packet longer than `cap` is dropped by the port, not truncated. */
    int ( *recv )( void *ctx, uint8_t *buf, uint16_t cap, uint32_t timeout_ms );
    /** Optional: used only to pace an upload burst (vftp_client_cfg_t::tx_gap_ms). */
    void ( *sleep_ms )( void *ctx, uint32_t ms );
    void *ctx;       /**< passed to every function above */
    uint16_t mtu_tx; /**< largest vftp packet client -> server */
    uint16_t mtu_rx; /**< largest vftp packet server -> client */
} vftp_link_t;

#ifdef __cplusplus
}
#endif
#endif /* VFTP_LINK_H */
