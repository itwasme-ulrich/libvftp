/**
 * @file vftp_client.h
 * @brief vftp_burst and vftp_poll client -- transport and OS agnostic.
 *
 * The client drives a server over a vftp_link_t and reads/writes its own local
 * files through a vftp_storage_t, so it runs anywhere a link and a storage port
 * exist: a ground PC over CSP or CCSDS, an OBC pulling a file from a payload,
 * a unit test in one process.
 *
 * Robustness rules every call follows:
 * - a lone control round trip (request -> reply) is retried `retries` times,
 *   and the server answers a repeated request idempotently;
 * - packets that are not the awaited reply (a late DATA from the previous
 *   round, a duplicate) are skipped, not treated as an error;
 * - burst runs in rounds of at most `window` chunks; it gives up only after
 *   `max_stall` consecutive rounds without any progress, never after a fixed
 *   number of rounds -- so a long, lossy transfer that keeps making progress
 *   always finishes;
 * - a transfer is reported successful only after the end-to-end CRC-32 of
 *   the whole file matched.
 *
 * Every call is blocking and runs in the caller's thread; one vftp_client_t serves one call
 * at a time (use one instance per thread).
 */
#ifndef VFTP_CLIENT_H
#define VFTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "vftp_config.h"
#include "vftp_link.h"
#include "vftp_storage.h"
#include "vftp_types.h"
#include "vftp_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Progress notifications delivered to vftp_client_cfg_t::on_event. */
typedef enum {
    VFTP_EV_START,    /**< size, crc32 (0 = not known yet), chunk_size, total_chunks are known */
    VFTP_EV_RESUME,   /**< done_chunks were already transferred by an earlier attempt */
    VFTP_EV_PROGRESS, /**< one more chunk landed: done_chunks, bytes_done */
    VFTP_EV_ROUND,    /**< burst: a round ended: round, done_chunks, missing */
    VFTP_EV_CRC,      /**< remote_crc vs local_crc */
    VFTP_EV_VERIFY    /**< the server is still checksumming a large file: bytes_done of size */
} vftp_event_type_t;

/** @brief One event; only the fields named in vftp_event_type_t are meaningful. */
typedef struct {
    vftp_event_type_t type; /**< what happened */
    uint32_t size; /**< file size */
    uint32_t crc32; /**< file CRC-32 (0 = not known yet) */
    uint16_t chunk_size; /**< chunk size */
    uint32_t total_chunks; /**< chunks in the file */
    uint32_t done_chunks; /**< chunks transferred so far */
    uint32_t bytes_done; /**< bytes transferred / checked so far */
    uint32_t round; /**< burst round number */
    uint32_t missing; /**< chunks still missing */
    uint32_t remote_crc; /**< CRC-32 reported by the server */
    uint32_t local_crc; /**< CRC-32 of the local file */
} vftp_event_t;

/**
 * @brief Event callback.
 * @param ctx vftp_client_cfg_t::event_ctx
 * @param ev the event (valid only during the call)
 */
typedef void ( *vftp_event_fn )( void *ctx, const vftp_event_t *ev );

/** @brief Client settings; start from vftp_client_default_cfg(). */
typedef struct {
    const vftp_link_t *link;   /**< the service this call talks to (burst or poll) */
    const vftp_storage_t *fs;  /**< local files */
    uint16_t chunk_size;       /**< 0: the largest the link allows in the data's direction */
    uint32_t timeout_ms;       /**< one request -> reply round trip */
    uint32_t gap_ms;           /**< burst download: silence that ends a round */
    uint32_t window;           /**< burst: chunks per round (download; upload too unless window_up is set) */
    uint32_t window_up;        /**< burst upload: chunks per round, 0 = window. Keep it below the SERVER's receive
                                * queue that ends the round, and every round then costs a full timeout. */
    uint32_t tx_gap_ms;        /**< burst upload: pause between DATA packets (needs link->sleep_ms) */
    uint8_t retries;           /**< attempts for one control round trip / one poll chunk */
    uint8_t max_stall;         /**< burst: rounds in a row without progress before giving up */
    vftp_event_fn on_event;    /**< progress callback, may be NULL */
    void *event_ctx;           /**< passed to on_event */
} vftp_client_cfg_t;

/**
 * @brief Fills safe defaults: timeout 10000 ms, gap 1000 ms, window 64, window_up 0 (= window),
 *        retries 3, max_stall 4, chunk_size 0 (largest), tx_gap 0. `link` and `fs` stay NULL.
 * @param[out] cfg settings to fill
 */
void vftp_client_default_cfg( vftp_client_cfg_t *cfg );

/** @brief Counters of the last call. */
typedef struct {
    uint32_t data_tx;     /**< DATA packets sent (upload) */
    uint32_t data_rx;     /**< DATA packets received (download), duplicates included */
    uint32_t duplicates;  /**< DATA packets that carried a chunk already held */
    uint32_t rounds;      /**< burst rounds / poll chunk exchanges */
    uint32_t retries;     /**< control or chunk retransmissions */
} vftp_client_stats_t;

/**
 * @brief Client instance. Large (bitmap sized by #VFTP_CFG_CLIENT_MAX_CHUNKS): allocate it
 *        statically, never on a small task stack.
 */
typedef struct {
    vftp_client_cfg_t cfg;       /**< copy of the settings passed to vftp_client_init() */
    uint8_t remote_ret;          /**< server's code when a call returned VFTP_REMOTE */
    vftp_client_stats_t stats;   /**< of the last call */
    uint8_t session;             /**< session byte of the transfer in progress (0 = none yet) */
    vftp_msg_t msg;              /**< scratch */
    uint8_t tx[VFTP_CFG_PACKET_MAX];       /**< scratch */
    uint8_t rx[VFTP_CFG_PACKET_MAX];       /**< scratch */
    uint8_t buf[VFTP_CFG_CLIENT_BUF_SIZE]; /**< local CRC / resume scratch */
    uint8_t bitmap[( VFTP_CFG_CLIENT_MAX_CHUNKS + 7u ) / 8u]; /**< burst download: chunks held */
} vftp_client_t;

/**
 * @brief Prepares a client. Settings may also be changed later through c->cfg.
 * @param[out] c instance
 * @param cfg settings (copied)
 * @return VFTP_OK, VFTP_INVAL if an argument is NULL
 */
vftp_ret_t vftp_client_init( vftp_client_t *c, const vftp_client_cfg_t *cfg );

/**
 * @brief Burst download, resumable: a sidecar "XXXXXXXX.VFD" next to `local` remembers the
 *        chunks held, so an interrupted call continues where it stopped (also with another
 *        chunk_size). The sidecar is ignored (fresh start) if it describes another remote
 *        file or `local` is missing or shorter than the chunks it marks held. Success only after
 *        the end-to-end CRC-32 matched.
 * @param c client (cfg.link = a burst link)
 * @param remote path on the server (<= 55 chars)
 * @param local local path
 * @return VFTP_OK; VFTP_REMOTE (see c->remote_ret: NOENT, ISDIR, BUSY...); VFTP_TIMEOUT;
 *         VFTP_LINK; VFTP_CRC_MISMATCH (resume state discarded: call again); VFTP_NOSPC (file
 *         needs more chunks than VFTP_CFG_CLIENT_MAX_CHUNKS); VFTP_ISDIR (`local` is a local
 *         directory: refused before the server is asked); local storage errors. A failure on this
 *         side after the server opened the session sends it ABORT, so the service is free at once.
 */
vftp_ret_t vftp_burst_download( vftp_client_t *c, const char *remote, const char *local );

/**
 * @brief Burst upload, resumable: the server keeps a sidecar "XXXXXXXX.VFM" in its root.
 * @param c client (cfg.link = a burst link)
 * @param local local file
 * @param remote path on the server; refused up front with VFTP_BADNAME / VFTP_ISDIR if the
 *        server's filesystem cannot hold it
 * @return as vftp_burst_download(); VFTP_REMOTE + VFTP_IO when the server's storage keeps failing
 */
vftp_ret_t vftp_burst_upload( vftp_client_t *c, const char *local, const char *remote );

/**
 * @brief Deletes a remote file and its resume state.
 * @param c client (burst link)
 * @param remote path on the server
 * @return VFTP_OK, VFTP_REMOTE (NOENT, BUSY if that file is being transferred), VFTP_TIMEOUT, VFTP_LINK
 */
vftp_ret_t vftp_burst_remove( vftp_client_t *c, const char *remote );

/**
 * @brief Listing callback.
 * @param ctx cb_ctx of vftp_burst_list()
 * @param total entries in the directory
 * @param index position of this entry (0..total-1, the order every listing method uses)
 * @param entry the entry (valid only during the call)
 */
typedef void ( *vftp_list_fn )( void *ctx, uint32_t total, uint32_t index, const vftp_list_entry_t *entry );

/**
 * @brief Listing method 2: pages through `dir`, calling `cb` once per entry. The server ends
 *        the conversation with the last page, so the service is free again at once.
 * @param c client (burst link)
 * @param dir directory ("" or "/" = root)
 * @param cb per-entry callback, may be NULL (count only)
 * @param cb_ctx passed to cb
 * @param[out] total entries, may be NULL
 * @return VFTP_OK, VFTP_REMOTE + NOENT (no such directory), VFTP_TIMEOUT, VFTP_LINK
 */
vftp_ret_t vftp_burst_list( vftp_client_t *c, const char *dir, vftp_list_fn cb, void *cb_ctx, uint32_t *total );

/**
 * @brief Listing method 1: the server writes #VFTP_LIST_TXT ("LIST.TXT", in its root, whatever
 *        `dir`) -- a "# <dir>" line, one "size date name[/]" line per entry, a final
 *        "# <n> entries" line. Fetch it afterwards with vftp_burst_download().
 * @param c client (burst link)
 * @param dir directory to list
 * @param[out] out_path receives the path of the generated file (>= #VFTP_MAX_PATH bytes), may be NULL
 * @return VFTP_OK, VFTP_REMOTE (NOENT, BUSY, IO), VFTP_TIMEOUT, VFTP_LINK
 */
vftp_ret_t vftp_burst_list_regen( vftp_client_t *c, const char *dir, char *out_path );

/**
 * @brief Listing method 3 (burst service): entry count, or the n-th entry, of a directory.
 *
 * Addresses files by their position -- the same index `ls` shows -- without knowing names.
 * @param c client (burst link)
 * @param dir directory
 * @param index entry wanted, or #VFTP_INDEX_COUNT_ONLY (then info->total is the answer)
 * @param[out] info the entry (name, size, flags, mtime) and total
 * @return VFTP_OK; VFTP_REMOTE with remote_ret VFTP_RANGE (index >= total) or VFTP_NOENT
 */
vftp_ret_t vftp_burst_index( vftp_client_t *c, const char *dir, uint32_t index, vftp_index_rep_t *info );

/**
 * @brief Listing method 3 on the poll service. Same contract as vftp_burst_index().
 * @param c client (poll link)
 * @param dir directory
 * @param index entry wanted, or #VFTP_INDEX_COUNT_ONLY
 * @param[out] info the entry and total
 * @return as vftp_burst_index()
 */
vftp_ret_t vftp_poll_index( vftp_client_t *c, const char *dir, uint32_t index, vftp_index_rep_t *info );

/**
 * @brief Resolves the n-th entry of `dir`, then burst-downloads "<dir>/<name>".
 * @param c client (burst link)
 * @param dir directory
 * @param index entry position
 * @param local destination, NULL = the remote name
 * @param[out] remote_out resolved remote path (>= #VFTP_MAX_PATH bytes), may be NULL
 * @return as vftp_burst_download(); VFTP_ISDIR if the entry is a directory (nothing downloaded)
 */
vftp_ret_t vftp_burst_download_index( vftp_client_t *c, const char *dir, uint32_t index, const char *local,
                                      char *remote_out );

/**
 * @brief Poll-service variant of vftp_burst_download_index().
 * @param c client (poll link)
 * @param dir directory
 * @param index entry position
 * @param local destination, NULL = the remote name
 * @param[out] remote_out resolved remote path, may be NULL
 * @return as vftp_poll_download(); VFTP_ISDIR if the entry is a directory
 */
vftp_ret_t vftp_poll_download_index( vftp_client_t *c, const char *dir, uint32_t index, const char *local,
                                     char *remote_out );

/**
 * @brief Poll download: stop-and-wait, one chunk per round trip. Resumable: a sidecar
 *        "XXXXXXXX.VFP" next to `local` keeps a watermark (chunks 0..n-1 already written, saved
 *        every 16 chunks and when the call fails), so a new call for the same remote file goes on
 *        from there, also with another chunk_size. The watermark is ignored (fresh start, `local`
 *        truncated) if the remote size or CRC differs or `local` is shorter than it claims; it is
 *        removed on success and on a CRC mismatch. Like burst's .VFD, the sidecar needs room for
 *        one more local file: if it cannot be created, the call fails with that storage error.
 * @param c client (poll link)
 * @param remote path on the server
 * @param local local path
 * @return as vftp_burst_download()
 */
vftp_ret_t vftp_poll_download( vftp_client_t *c, const char *remote, const char *local );

/**
 * @brief Poll upload: stop-and-wait, resumes from the server's watermark ("XXXXXXXX.VFI" in
 *        its root), also across a chunk_size change.
 * @param c client (poll link)
 * @param local local file
 * @param remote path on the server
 * @return as vftp_burst_upload()
 */
vftp_ret_t vftp_poll_upload( vftp_client_t *c, const char *local, const char *remote );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_CLIENT_H */
