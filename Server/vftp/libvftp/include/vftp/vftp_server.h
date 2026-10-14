/**
 * @file vftp_server.h
 * @brief vftp_burst and vftp_poll server engines -- transport and OS agnostic.
 *
 * A server instance is a plain struct the caller allocates (statically, on a
 * target with no allocation after boot); nothing is global, so one process can
 * run several instances (one per transport, per service, per test).
 *
 * The caller's transport loop does exactly three things:
 * 1. every received packet -> vftp_xxx_server_handle(); replies go out
 *    through the `send` callback (zero, one, or -- burst download -- many);
 * 2. when handle() returns VFTP_SRV_CLOSE, end the conversation (close the
 *    CSP connection); a connectionless transport just ignores it;
 * 3. when the conversation ends for ANY other reason (peer gone, idle
 *    timeout, connection reset), call vftp_xxx_server_abort().
 *
 * Rule 3 is what frees the service for the next client; abort() is idempotent
 * and keeps upload resume state.
 *
 * A connectionless transport (UDP, CCSDS Space Packets, CSP without RDP) has
 * no "connection ended" event, so its loop applies rule 3 itself: if a
 * service is busy and its `activity` counter has not moved for the port's idle
 * time, abort it. `activity` only counts packets that belong to the open
 * session -- a new request refused with BUSY does not keep a dead session alive.
 */
#ifndef VFTP_SERVER_H
#define VFTP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "vftp_config.h"
#include "vftp_crc32.h"
#include "vftp_storage.h"
#include "vftp_types.h"
#include "vftp_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Puts one reply packet on the wire (provided by the transport port).
 *
 * May block for flow control (wait for a buffer, a radio slot, an RDP window).
 * @param ctx send_ctx given to handle() (e.g. the CSP connection)
 * @param pkt packet bytes (valid only during the call: copy them)
 * @param len packet length, never more than vftp_server_cfg_t::mtu_tx
 * @return 0 = sent; anything else stops a burst early (no buffer, link gone) -- the client
 *         simply asks again next round
 */
typedef int ( *vftp_send_fn )( void *ctx, const uint8_t *pkt, uint16_t len );

/** @brief Server settings. */
typedef struct {
    const vftp_storage_t *fs; /**< the files this server serves */
    uint16_t mtu_tx; /**< largest packet this server may send (bounds download chunk_size, reply sizes) */
    uint16_t mtu_rx; /**< largest packet the client may send (bounds upload chunk_size) */
} vftp_server_cfg_t;

/** @brief What the transport loop must do after handle(). */
typedef enum {
    VFTP_SRV_KEEP = 0, /**< conversation continues (or nothing to decide) */
    VFTP_SRV_CLOSE = 1 /**< exchange finished: end the conversation */
} vftp_srv_action_t;

/* ---- burst ---------------------------------------------------------------- */

/**
 * @brief Burst server instance. Large (resume bitmap + 32 KiB buffer by default): allocate it
 *        statically. Fields are internal except crc_step, which a port may tune after init.
 */
typedef struct {
    vftp_server_cfg_t cfg; /**< settings given to init() */
    uint8_t state; /**< IDLE / DOWNLOAD / UPLOAD */
    bool staged; /**< upload is held whole in buf */
    char path[VFTP_MAX_PATH]; /**< file of the open transfer */
    char map_path[VFTP_SIDECAR_PATH_MAX]; /**< its upload sidecar (.VFM) */
    uint32_t size; /**< file size */
    uint32_t crc32; /**< CRC-32 announced by the client (upload) */
    uint32_t total; /**< chunks in the file */
    uint32_t complete; /**< chunks held (upload) */
    uint16_t chunk_size; /**< chunk size of the open transfer */
    uint32_t activity; /**< packets that belonged to the open session (see file header) */
    uint8_t session;      /**< session byte of the open transfer (0 = none); stale packets carry another */
    uint8_t last_session; /**< last session byte handed out (1..255, wraps) */
    uint32_t run_first; /**< upload write-behind: chunks [run_first, run_first+run_count) sit in buf */
    uint32_t run_count; /**< chunks in the pending run */
    uint32_t cache_off; /**< download read-ahead: file bytes [cache_off, cache_off+cache_len) sit in buf */
    uint32_t cache_len; /**< bytes in the read-ahead cache */
    uint32_t crc_step;  /**< most file bytes one request reads for the CRC: init sets VFTP_CFG_CRC_STEP_BYTES,
                         * a port may change it afterwards (slow card, tight watchdog); 0 = no limit */
    vftp_crcjob_t crc;  /**< file CRC: fed by download reads, finished by CRC_REQUESTs */
    uint8_t io_err;     /**< upload: last storage write error, and how many failed in a row */
    uint8_t io_streak; /**< storage writes failed in a row */
    char list_dir[VFTP_MAX_PATH]; /**< LIST paging: total of this directory, counted on page 0 */
    uint32_t list_total; /**< cached total of list_dir */
    bool list_valid; /**< list_total may be reused */
    uint16_t list_bytes; /**< LIST_REPLY being packed: bytes used so far */
    bool list_full;      /**< LIST_REPLY being packed: the next entry did not fit, page closed */
    vftp_msg_t rx; /**< last decoded packet */
    vftp_msg_t tx; /**< reply being encoded */
    uint8_t pkt[VFTP_CFG_PACKET_MAX]; /**< encode buffer */
    uint8_t bitmap[( VFTP_CFG_BURST_MAX_CHUNKS + 7u ) / 8u]; /**< upload: chunks received */
    uint8_t buf[VFTP_CFG_BURST_BUF_SIZE]; /**< staging / write-behind / read-ahead / CRC scratch */
} vftp_burst_server_t;

/**
 * @brief Prepares a burst server.
 * @param[out] s instance
 * @param cfg settings (copied); mtu_tx must hold a LIST_REPLY with one entry (>= 35),
 *        mtu_rx a DATA header plus 1 byte, both <= #VFTP_CFG_PACKET_MAX
 * @return VFTP_OK or VFTP_INVAL
 */
vftp_ret_t vftp_burst_server_init( vftp_burst_server_t *s, const vftp_server_cfg_t *cfg );

/**
 * @brief Handles one received packet (rule 1 of the file description).
 *
 * Runs to completion in the caller's thread; may read/write storage and call `send`
 * zero, one or many times. A malformed packet is ignored (VFTP_SRV_KEEP).
 * @param s instance
 * @param pkt packet bytes (vftp only, transport header removed)
 * @param len packet length
 * @param send reply function
 * @param send_ctx passed to send
 * @return VFTP_SRV_CLOSE when the exchange is over (end the conversation), else VFTP_SRV_KEEP
 */
vftp_srv_action_t vftp_burst_server_handle( vftp_burst_server_t *s, const uint8_t *pkt, uint16_t len,
                                            vftp_send_fn send, void *send_ctx );

/**
 * @brief The conversation ended without DONE (rule 3): flushes write-behind data, keeps the
 *        upload's resume sidecar, frees the service. Idempotent.
 * @param s instance
 */
void vftp_burst_server_abort( vftp_burst_server_t *s );

/**
 * @brief True while a transfer is open (the port uses its long idle timeout then).
 * @param s instance
 * @return transfer open
 */
bool vftp_burst_server_busy( const vftp_burst_server_t *s );

/* ---- poll ----------------------------------------------------------------- */

/** @brief Poll server instance (small: no bitmap). Fields are internal except crc_step. */
typedef struct {
    vftp_server_cfg_t cfg; /**< settings given to init() */
    uint8_t state; /**< IDLE / DOWNLOAD / UPLOAD */
    char path[VFTP_MAX_PATH]; /**< file of the open transfer */
    char idx_path[VFTP_SIDECAR_PATH_MAX]; /**< its upload sidecar (.VFI) */
    uint32_t size; /**< file size */
    uint32_t crc32; /**< CRC-32 announced by the client (upload) */
    uint32_t total; /**< chunks in the file */
    uint32_t committed; /**< upload: chunks 0..committed-1 are on storage */
    uint16_t chunk_size; /**< chunk size of the open transfer */
    uint32_t activity; /**< packets that belonged to the open session (see file header) */
    uint8_t session;      /**< session byte of the open transfer (0 = none); stale packets carry another */
    uint8_t last_session; /**< last session byte handed out (1..255, wraps) */
    uint32_t crc_step;  /**< as in vftp_burst_server_t */
    vftp_crcjob_t crc;  /**< file CRC job */
    uint8_t io_streak;  /**< upload: storage writes failed in a row */
    vftp_msg_t rx; /**< last decoded packet */
    vftp_msg_t tx; /**< reply being encoded */
    uint8_t pkt[VFTP_CFG_PACKET_MAX]; /**< encode buffer */
    uint8_t buf[VFTP_CFG_POLL_BUF_SIZE]; /**< CRC scratch */
} vftp_poll_server_t;

/**
 * @brief Prepares a poll server.
 * @param[out] s instance
 * @param cfg settings (copied); both MTUs > #VFTP_DATA_HEADER and <= #VFTP_CFG_PACKET_MAX
 * @return VFTP_OK or VFTP_INVAL
 */
vftp_ret_t vftp_poll_server_init( vftp_poll_server_t *s, const vftp_server_cfg_t *cfg );

/**
 * @brief Handles one received packet; at most one reply. Same contract as vftp_burst_server_handle().
 * @param s instance
 * @param pkt packet bytes
 * @param len packet length
 * @param send reply function
 * @param send_ctx passed to send
 * @return VFTP_SRV_CLOSE when the exchange is over, else VFTP_SRV_KEEP
 */
vftp_srv_action_t vftp_poll_server_handle( vftp_poll_server_t *s, const uint8_t *pkt, uint16_t len,
                                           vftp_send_fn send, void *send_ctx );

/**
 * @brief The conversation ended without DONE: frees the service, keeps the upload watermark. Idempotent.
 * @param s instance
 */
void vftp_poll_server_abort( vftp_poll_server_t *s );

/**
 * @brief True while a transfer is open.
 * @param s instance
 * @return transfer open
 */
bool vftp_poll_server_busy( const vftp_poll_server_t *s );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_SERVER_H */
