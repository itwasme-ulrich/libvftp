/* libvftp private helpers used only by the burst and poll servers (src/server/vftp_server_util.c)
 * -- not installed, not part of the API. A client-only build never sees this header. */
#ifndef VFTP_SERVER_INTERNAL_H
#define VFTP_SERVER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../common/vftp_internal.h"

#if VFTP_CFG_BURST_BUF_SIZE < VFTP_CFG_PACKET_MAX
#error "VFTP_CFG_BURST_BUF_SIZE must hold at least one packet"
#endif

/* One pass over `dir` (walk() if the port has it, else dirent()): *total = entry count, and fn is
 * called for entries [first, first+max) (max 0 = none). If `stop_early` and the port walks, the
 * pass stops after the range and *total is left untouched (caller already knows it).
 * VFTP_OK, or the port's status for a missing directory. */
vftp_ret_t vftp_dir_scan( const vftp_storage_t *fs, const char *dir, uint32_t first, uint32_t max, vftp_walk_fn fn,
                          void *arg, uint32_t *total, bool stop_early );

/* Answers an INDEX_REQUEST (both services): total entries of q->path and, unless q->index is
 * VFTP_INDEX_COUNT_ONLY, the q->index-th entry. */
void vftp_index_lookup( const vftp_storage_t *fs, const vftp_index_req_t *q, vftp_index_rep_t *r );

/* Incremental CRC-32 of a file on storage, for files too large to checksum in one go: fed for free
 * while data is read/written in order, finished in bounded steps (CRC_REPLY ret AGAIN) when not. */
void vftp_crcjob_init( vftp_crcjob_t *j, uint32_t size );
/* Bytes [off, off+len) as stored: absorbed if they continue the job exactly, ignored otherwise. */
void vftp_crcjob_feed( vftp_crcjob_t *j, uint32_t off, const uint8_t *data, uint32_t len );
/* Reads up to `budget` (0: no limit) more bytes of `path` through buf. VFTP_OK whether or not that
 * finished it. */
vftp_ret_t vftp_crcjob_step( vftp_crcjob_t *j, const vftp_storage_t *fs, const char *path, uint8_t *buf,
                             uint32_t buf_size, uint32_t budget );

/* VFTP_BADNAME if the port's name_ok() refuses `path`, else VFTP_OK. Checked for EVERY path a client
 * names (download, delete, list, list_regen, index, upload), not only for uploads: a FAT port without
 * long names silently cuts a longer name to 8.3 and would serve or delete ANOTHER file (bench,
 * 2026-10-08: "VT/R00009998" downloaded VT/R0000999, `rm TR/X.ABCD` deleted TR/X.ABC). */
vftp_ret_t vftp_client_path_ok( const vftp_storage_t *fs, const char *path );

/* UPLOAD_REQUEST target check, before anything is truncated: VFTP_INVAL for an empty path or one
 * ending in '/', VFTP_ISDIR for an existing directory, the port's name_ok() verdict (VFTP_BADNAME). */
vftp_ret_t vftp_upload_target_ok( const vftp_storage_t *fs, const char *path );

/* Exact string compare. */
bool vftp_str_eq( const char *a, const char *b );
/* Case-insensitive ASCII compare (FAT names). */
bool vftp_str_ieq( const char *a, const char *b );

#endif /* VFTP_SERVER_INTERNAL_H */
