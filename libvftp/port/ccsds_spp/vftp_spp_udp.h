/**
 * @file vftp_spp_udp.h
 * @brief vftp over CCSDS Space Packets in UDP datagrams (POSIX sockets), with
 *        optional simulated loss -- a second, non-CSP transport for libvftp.
 */
#ifndef VFTP_SPP_UDP_H
#define VFTP_SPP_UDP_H

#include <netinet/in.h>
#include <sys/socket.h>

#include "vftp/vftp_link.h"
#include "vftp/vftp_server.h"
#include "vftp_spp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Client side: one UDP socket talking to one service (APID). */
typedef struct {
    int fd;          /**< UDP socket */
    uint16_t apid;   /**< service: VFTP_SPP_APID_BURST or VFTP_SPP_APID_POLL */
    uint16_t seq;    /**< next sequence count */
    uint16_t loss_tx; /**< per mille, simulated */
    uint16_t loss_rx; /**< per mille, simulated */
    uint32_t seed; /**< loss simulation seed */
    /** Statistics. @{ */
    uint32_t sent, received, dropped_tx, dropped_rx;
    /** @} */
    uint8_t frame[VFTP_CFG_PACKET_MAX + VFTP_SPP_HEADER]; /**< scratch */
} vftp_spp_udp_client_t;

/**
 * @brief Opens the client socket and fills `link`.
 * @param[out] link client link to fill
 * @param[out] c client state
 * @param host server host name or address
 * @param port server UDP port
 * @param apid service APID
 * @param mtu_tx largest vftp packet client -> server
 * @param mtu_rx largest vftp packet server -> client
 * @return 0, or -1 on a socket/address error
 */
int vftp_spp_udp_client_open( vftp_link_t *link, vftp_spp_udp_client_t *c, const char *host, uint16_t port,
                              uint16_t apid, uint16_t mtu_tx, uint16_t mtu_rx );

/**
 * @brief Closes the client socket.
 * @param c client state
 */
void vftp_spp_udp_client_close( vftp_spp_udp_client_t *c );

/**
 * @brief Server side: one UDP socket serving both services, selected by APID. Applies rule 3
 *        of vftp_server.h itself (connectionless): an open transfer whose activity counter
 *        does not move for idle_ms is aborted.
 */
typedef struct {
    int fd; /**< UDP socket */
    vftp_burst_server_t *burst; /**< burst engine (may be NULL) */
    vftp_poll_server_t *poll;   /**< poll engine (may be NULL) */
    uint16_t apid_burst; /**< APID routed to `burst` */
    uint16_t apid_poll;  /**< APID routed to `poll` */
    uint32_t idle_ms;  /**< abort an open transfer whose activity counter has not moved for this long */
    uint16_t loss_tx;  /**< per mille, simulated */
    uint16_t loss_rx; /**< per mille, simulated */
    uint32_t seed;      /**< loss simulation seed */
    int verbose;        /**< log sessions to stderr */
    volatile int stop;  /**< set to make vftp_spp_udp_server_run() return */
    /** Statistics. @{ */
    uint32_t rx, tx, dropped_tx, dropped_rx, bad;
    /** @} */
    struct sockaddr_storage peer; /**< last client address (replies go there) */
    socklen_t peer_len;           /**< size of peer */
    uint16_t reply_apid;          /**< APID of the packet being answered */
    uint16_t seq;                 /**< next sequence count */
    uint8_t frame[VFTP_CFG_PACKET_MAX + VFTP_SPP_HEADER]; /**< scratch */
} vftp_spp_udp_server_t;

/**
 * @brief Binds UDP `port` on all interfaces.
 * @param s server state (burst/poll and apids set by the caller)
 * @param port UDP port
 * @return 0, or -1 on a socket error
 */
int vftp_spp_udp_server_open( vftp_spp_udp_server_t *s, uint16_t port );

/**
 * @brief Serves until s->stop is set.
 * @param s server state
 */
void vftp_spp_udp_server_run( vftp_spp_udp_server_t *s );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_SPP_UDP_H */
