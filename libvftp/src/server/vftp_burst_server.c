/* libvftp: vftp_burst server engine.
 *
 * DOWNLOAD: the client sends the {next,count} ranges it still needs (a
 * STATUS_REPLY-shaped message); every chunk in them goes out back to back.
 * Chunks are served from a read-ahead cache in `buf`, so a storage port with
 * expensive open/seek (FAT on SD) pays one read per VFTP_CFG_BURST_BUF_SIZE
 * bytes, not one per chunk. The file CRC is computed at the request when the
 * file is small (<= VFTP_CFG_CRC_STEP_BYTES); a larger file is checksummed
 * from the very reads that serve it, and whatever those did not cover (a
 * resumed download skips chunks) is read at CRC_REQUEST, at most
 * VFTP_CFG_CRC_STEP_BYTES per request (reply VFTP_AGAIN until done) -- no
 * single packet ever keeps the server busy for long, whatever the file size.
 *
 * UPLOAD, two write paths chosen by size at UPLOAD_REQUEST:
 *   staged (size <= VFTP_CFG_STAGE_MAX_BYTES and chunks <= VFTP_CFG_STAGE_MAX_CHUNKS):
 *                          chunks land in `buf`; the file is truncated and
 *                          written in one go when the last chunk arrives. No
 *                          sidecar -- a lost link loses the RAM copy, and a
 *                          file this small is simply sent again (both bounds
 *                          keep that cheap on a slow radio link).
 *   write-behind (larger): contiguous chunks accumulate in `buf` and are
 *                          written as one run when the run breaks, the buffer
 *                          fills, or the client asks for STATUS/CRC/DONE. A
 *                          chunk's bit reaches the sidecar only AFTER its run
 *                          is on storage, so the persisted bitmap never claims
 *                          data that is not durable. The in-RAM bitmap (what
 *                          STATUS reports) is set on receipt.
 * A storage write that keeps failing (VFTP_CFG_IO_FAIL_LIMIT in a row) is
 * reported in STATUS_REPLY.ret, so the client stops instead of resending
 * forever into a full or broken card. The upload CRC is read back from
 * storage, never taken from RAM: it proves what is really on the card.
 *
 * Sidecar ("XXXXXXXX.VFM" in the root, vftp_sidecar_path): 24-byte header
 *   magic:4 version:2 chunk_size:2 size:4 crc32:4 total:4 path_crc:4
 * then (total+7)/8 bitmap bytes. All integers little-endian. A resume with a
 * different chunk_size keeps every byte already held (vftp_rechunk).
 */
#include <stdio.h>
#include <string.h>

#include "vftp/vftp_crc32.h"
#include "vftp/vftp_server.h"
#include "vftp_server_internal.h"

enum { ST_IDLE = 0, ST_DOWNLOAD = 1, ST_UPLOAD = 2 };

#define MAP_MAGIC 0x4D465456u /* "VTFM" */
#define MAP_VERSION 1u
#define MAP_HEADER 24u

/* ---- small helpers ------------------------------------------------------- */

static int send_tx( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    uint16_t n = vftp_encode( &s->tx, s->pkt, s->cfg.mtu_tx );
    if ( n == 0u ) {
        VFTP_LOG_WARN( "vftp_burst: reply type %u does not fit mtu %u", (unsigned int) s->tx.type,
                       (unsigned int) s->cfg.mtu_tx );
        return -1;
    }
    return send( ctx, s->pkt, n );
}

/* A reply that carries only a result. Its session byte: the open session when it accepts; 0 when
 * it refuses to open one (DOWNLOAD/UPLOAD_REPLY); otherwise the request's own, so the client
 * matches the refusal to what it asked. */
static void reply_ret( vftp_burst_server_t *s, uint8_t type, vftp_ret_t ret, vftp_send_fn send, void *ctx )
{
    memset( &s->tx.u, 0, sizeof( s->tx.u ) );
    s->tx.type = type;
    if ( ret == VFTP_OK ) {
        s->tx.session = s->session;
    } else if ( type == VFTP_MSG_DOWNLOAD_REPLY || type == VFTP_MSG_UPLOAD_REPLY ) {
        s->tx.session = 0u;
    } else {
        s->tx.session = s->rx.session;
    }
    switch ( type ) {
        case VFTP_MSG_DOWNLOAD_REPLY:
            s->tx.u.down_rep.ret = (uint8_t) ret;
            break;
        case VFTP_MSG_UPLOAD_REPLY:
            s->tx.u.up_rep.ret = (uint8_t) ret;
            break;
        case VFTP_MSG_CRC_REPLY:
            s->tx.u.crc_rep.ret = (uint8_t) ret;
            break;
        case VFTP_MSG_STATUS_REPLY:
            s->tx.u.status.ret = (uint8_t) ret;
            break;
        case VFTP_MSG_LIST_REPLY:
            s->tx.u.list_rep.ret = (uint8_t) ret;
            break;
        default: /* DELETE_REPLY, LIST_REGEN_REPLY */
            s->tx.u.ret_rep.ret = (uint8_t) ret;
            break;
    }
    (void) send_tx( s, send, ctx );
}

/* Next session byte: 1..255, never 0 (0 = no session). */
static uint8_t new_session( vftp_burst_server_t *s )
{
    s->last_session = (uint8_t) ( s->last_session == 255u ? 1u : s->last_session + 1u );
    return s->last_session;
}

static void session_reset( vftp_burst_server_t *s )
{
    s->state = ST_IDLE;
    s->session = 0u;
    s->staged = false;
    s->path[0] = '\0';
    s->map_path[0] = '\0';
    s->size = 0;
    s->crc32 = 0;
    s->total = 0;
    s->complete = 0;
    s->chunk_size = 0;
    s->run_first = 0;
    s->run_count = 0;
    s->cache_off = 0;
    s->cache_len = 0;
    vftp_crcjob_init( &s->crc, 0u );
    s->io_err = VFTP_OK;
    s->io_streak = 0;
}

static uint32_t bitmap_bytes( uint32_t total )
{
    return ( total + 7u ) / 8u;
}

/* ---- upload: sidecar ----------------------------------------------------- */

static void map_header( const vftp_burst_server_t *s, uint8_t *h )
{
    vftp_put_u32( h + 0, MAP_MAGIC );
    vftp_put_u16( h + 4, MAP_VERSION );
    vftp_put_u16( h + 6, s->chunk_size );
    vftp_put_u32( h + 8, s->size );
    vftp_put_u32( h + 12, s->crc32 );
    vftp_put_u32( h + 16, s->total );
    vftp_put_u32( h + 20, vftp_crc32( s->path, strlen( s->path ) ) );
}

static vftp_ret_t map_create( vftp_burst_server_t *s );

/* Loads the sidecar into s->bitmap if it belongs to this transfer (same path, size and CRC). Saved
 * with another chunk_size, its bytes are carried over to the new chunking and the sidecar is
 * rewritten in the new form. */
static bool map_load( vftp_burst_server_t *s )
{
    const vftp_storage_t *fs = s->cfg.fs;
    uint8_t want[MAP_HEADER];
    uint8_t got_hdr[MAP_HEADER];
    uint16_t got = 0;
    map_header( s, want );
    if ( fs->read( fs->ctx, s->map_path, 0, got_hdr, (uint16_t) MAP_HEADER, &got ) != VFTP_OK || got != MAP_HEADER ||
         memcmp( want, got_hdr, 6u ) != 0 || memcmp( want + 8, got_hdr + 8, 8u ) != 0 ||
         memcmp( want + 20, got_hdr + 20, 4u ) != 0 ) {
        return false; /* magic/version, size+crc or path differ: another transfer's state */
    }
    uint16_t old_cs = vftp_get_u16( got_hdr + 6 );
    uint32_t old_total = vftp_get_u32( got_hdr + 16 );
    if ( old_cs == s->chunk_size ) {
        uint32_t nbytes = bitmap_bytes( s->total );
        uint32_t n = 0;
        if ( old_total != s->total ||
             vftp_fs_read_all( fs, s->map_path, MAP_HEADER, s->bitmap, nbytes, &n ) != VFTP_OK || n != nbytes ) {
            return false;
        }
        s->complete = vftp_bitmap_fix_count( s->bitmap, s->total );
        return true;
    }
    uint32_t held = 0;
    if ( old_cs == 0u || old_total != vftp_total_chunks( s->size, old_cs ) ||
         vftp_rechunk( fs, s->map_path, MAP_HEADER, old_cs, old_total, s->chunk_size, s->total, s->size, s->bitmap,
                       s->buf, (uint32_t) sizeof( s->buf ), &held ) != VFTP_OK ||
         map_create( s ) != VFTP_OK ) {
        return false;
    }
    s->complete = held;
    VFTP_LOG_INFO( "vftp_burst: %s chunk size %u -> %u, %lu/%lu chunks kept", s->path, (unsigned int) old_cs,
                   (unsigned int) s->chunk_size, (unsigned long) held, (unsigned long) s->total );
    return true;
}

static vftp_ret_t map_create( vftp_burst_server_t *s )
{
    const vftp_storage_t *fs = s->cfg.fs;
    uint8_t h[MAP_HEADER];
    map_header( s, h );
    vftp_ret_t st = fs->truncate( fs->ctx, s->map_path );
    if ( st == VFTP_OK ) {
        st = fs->write( fs->ctx, s->map_path, 0, h, (uint16_t) MAP_HEADER );
    }
    if ( st == VFTP_OK ) {
        st = vftp_fs_write_all( fs, s->map_path, MAP_HEADER, s->bitmap, bitmap_bytes( s->total ) );
    }
    return st;
}

/* Writes the pending run, then (only then) persists its bits. */
static vftp_ret_t flush_run( vftp_burst_server_t *s )
{
    if ( s->run_count == 0u ) {
        return VFTP_OK;
    }
    const vftp_storage_t *fs = s->cfg.fs;
    uint32_t first = s->run_first;
    uint32_t count = s->run_count;
    uint32_t off = first * (uint32_t) s->chunk_size;
    uint32_t len = count * (uint32_t) s->chunk_size;
    if ( len > s->size - off ) {
        len = s->size - off;
    }
    s->run_count = 0;

    vftp_ret_t st = vftp_fs_write_all( fs, s->path, off, s->buf, len );
    if ( st != VFTP_OK ) {
        /* Not durable: forget these chunks so STATUS asks for them again. */
        for ( uint32_t i = 0; i < count; i++ ) {
            vftp_bit_clear( s->bitmap, first + i );
        }
        s->complete -= count;
        s->io_err = (uint8_t) st;
        if ( s->io_streak < UINT8_MAX ) {
            s->io_streak++;
        }
        VFTP_LOG_WARN( "vftp_burst: write of %lu bytes at %lu failed (%d)", (unsigned long) len, (unsigned long) off,
                       (int) st );
        return st;
    }
    s->io_streak = 0;
    uint32_t b0 = first >> 3;
    uint32_t b1 = ( first + count - 1u ) >> 3;
    /* A failed bitmap write only costs a resend after a reboot; the data itself is safe. */
    (void) vftp_fs_write_all( fs, s->map_path, MAP_HEADER + b0, s->bitmap + b0, b1 - b0 + 1u );
    return VFTP_OK;
}

/* ---- download: read-ahead ------------------------------------------------ */

static vftp_ret_t read_chunk( vftp_burst_server_t *s, uint32_t chunk, uint8_t *dst, uint16_t *len )
{
    uint16_t n = vftp_chunk_len( s->size, s->chunk_size, chunk );
    uint32_t off = chunk * (uint32_t) s->chunk_size;
    if ( s->cache_len == 0u || off < s->cache_off || off + n > s->cache_off + s->cache_len ) {
        uint32_t want = s->size - off;
        if ( want > sizeof( s->buf ) ) {
            want = (uint32_t) sizeof( s->buf );
        }
        uint32_t got = 0;
        s->cache_len = 0;
        vftp_ret_t st = vftp_fs_read_all( s->cfg.fs, s->path, off, s->buf, want, &got );
        if ( st != VFTP_OK ) {
            return st;
        }
        if ( got < n ) {
            return VFTP_IO; /* the file shrank under us */
        }
        s->cache_off = off;
        s->cache_len = got;
        vftp_crcjob_feed( &s->crc, off, s->buf, got ); /* free CRC progress while serving in order */
    }
    memcpy( dst, s->buf + ( off - s->cache_off ), n );
    *len = n;
    return VFTP_OK;
}

/* ---- handlers ------------------------------------------------------------ */

static vftp_srv_action_t on_download_request( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    const char *path = s->rx.u.down_req.path;
    uint16_t cs = s->rx.u.down_req.chunk_size;

    if ( s->state != ST_IDLE ) {
        if ( s->state == ST_DOWNLOAD && cs == s->chunk_size && vftp_str_eq( path, s->path ) ) {
            /* The client lost our reply and asked again: same answer. */
            s->activity++;
            s->tx.type = VFTP_MSG_DOWNLOAD_REPLY;
            s->tx.session = s->session;
            s->tx.u.down_rep.ret = VFTP_OK;
            s->tx.u.down_rep.size = s->size;
            s->tx.u.down_rep.crc32 = s->crc.done ? s->crc.value : 0u;
            s->tx.u.down_rep.flags = s->crc.done ? VFTP_DL_CRC_VALID : 0u;
            (void) send_tx( s, send, ctx );
        } else {
            reply_ret( s, VFTP_MSG_DOWNLOAD_REPLY, VFTP_BUSY, send, ctx );
        }
        return VFTP_SRV_KEEP;
    }
    if ( cs == 0u || (uint32_t) cs + VFTP_DATA_HEADER > s->cfg.mtu_tx ) {
        reply_ret( s, VFTP_MSG_DOWNLOAD_REPLY, VFTP_INVAL, send, ctx );
        return VFTP_SRV_KEEP;
    }
    uint32_t size = 0;
    vftp_ret_t st = vftp_client_path_ok( s->cfg.fs, path );
    if ( st == VFTP_OK ) {
        st = s->cfg.fs->stat( s->cfg.fs->ctx, path, &size );
    }
    if ( st == VFTP_OK ) {
        session_reset( s );
        vftp_crcjob_init( &s->crc, size );
        st = vftp_crcjob_step( &s->crc, s->cfg.fs, path, s->buf, (uint32_t) sizeof( s->buf ), s->crc_step );
    }
    if ( st != VFTP_OK ) {
        session_reset( s );
        reply_ret( s, VFTP_MSG_DOWNLOAD_REPLY, st, send, ctx );
        return VFTP_SRV_KEEP;
    }
    s->state = ST_DOWNLOAD;
    s->session = new_session( s );
    memcpy( s->path, path, VFTP_MAX_PATH );
    s->size = size;
    s->chunk_size = cs;
    s->total = vftp_total_chunks( size, cs );
    VFTP_LOG_INFO( "vftp_burst: download %s, %lu bytes in %lu chunks", s->path, (unsigned long) size,
                   (unsigned long) s->total );

    s->tx.type = VFTP_MSG_DOWNLOAD_REPLY;
    s->tx.session = s->session;
    s->tx.u.down_rep.ret = VFTP_OK;
    s->tx.u.down_rep.size = size;
    s->tx.u.down_rep.crc32 = s->crc.done ? s->crc.value : 0u;
    s->tx.u.down_rep.flags = s->crc.done ? VFTP_DL_CRC_VALID : 0u;
    (void) send_tx( s, send, ctx );
    return VFTP_SRV_KEEP;
}

/* WANT, "send me these ranges": the burst itself. */
static vftp_srv_action_t on_want_ranges( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    if ( s->state != ST_DOWNLOAD ) {
        return VFTP_SRV_KEEP; /* stale or misplaced: nothing to send */
    }
    s->activity++;
    for ( uint16_t i = 0; i < s->rx.u.want.entries; i++ ) {
        uint32_t next = s->rx.u.want.range[i].next;
        uint32_t count = s->rx.u.want.range[i].count;
        if ( next >= s->total ) {
            continue;
        }
        if ( count > s->total - next ) {
            count = s->total - next;
        }
        for ( uint32_t j = 0; j < count; j++ ) {
            uint32_t chunk = next + j;
            uint16_t n = 0;
            vftp_ret_t st = read_chunk( s, chunk, s->pkt + VFTP_DATA_HEADER, &n );
            if ( st != VFTP_OK ) {
                VFTP_LOG_WARN( "vftp_burst: read chunk %lu failed (%d)", (unsigned long) chunk, (int) st );
                return VFTP_SRV_KEEP;
            }
            s->tx.type = VFTP_MSG_DATA;
            s->tx.session = s->session;
            s->tx.u.data.chunk = chunk;
            s->tx.u.data.bytes = s->pkt + VFTP_DATA_HEADER;
            s->tx.u.data.len = n;
            if ( send_tx( s, send, ctx ) != 0 ) {
                return VFTP_SRV_KEEP; /* transport refused: the client asks again next round */
            }
        }
    }
    return VFTP_SRV_KEEP;
}

static bool same_upload( const vftp_burst_server_t *s )
{
    return s->state == ST_UPLOAD && s->size == s->rx.u.up_req.size && s->crc32 == s->rx.u.up_req.crc32 &&
           s->chunk_size == s->rx.u.up_req.chunk_size && vftp_str_eq( s->path, s->rx.u.up_req.path );
}

static vftp_srv_action_t on_upload_request( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    if ( s->state != ST_IDLE ) {
        bool same = same_upload( s );
        if ( same ) {
            s->activity++;
        }
        reply_ret( s, VFTP_MSG_UPLOAD_REPLY, same ? VFTP_OK : VFTP_BUSY, send, ctx );
        return VFTP_SRV_KEEP;
    }
    uint16_t cs = s->rx.u.up_req.chunk_size;
    if ( cs == 0u || (uint32_t) cs + VFTP_DATA_HEADER > s->cfg.mtu_rx ) {
        reply_ret( s, VFTP_MSG_UPLOAD_REPLY, VFTP_INVAL, send, ctx );
        return VFTP_SRV_KEEP;
    }
    uint32_t size = s->rx.u.up_req.size;
    uint32_t total = vftp_total_chunks( size, cs );
    /* Staged only when a restart is cheap: few bytes AND few packets (vftp_config.h). Everything
     * else is write-behind with a sidecar, so an interrupted upload resumes. */
    bool staged = size <= VFTP_CFG_STAGE_MAX_BYTES && total <= VFTP_CFG_STAGE_MAX_CHUNKS;
    vftp_ret_t st = vftp_upload_target_ok( s->cfg.fs, s->rx.u.up_req.path );
    if ( st == VFTP_OK && !staged && total > VFTP_CFG_BURST_MAX_CHUNKS ) {
        st = VFTP_NOSPC;
    }
    if ( st != VFTP_OK ) {
        reply_ret( s, VFTP_MSG_UPLOAD_REPLY, st, send, ctx );
        return VFTP_SRV_KEEP;
    }

    session_reset( s );
    s->state = ST_UPLOAD;
    s->staged = staged;
    memcpy( s->path, s->rx.u.up_req.path, VFTP_MAX_PATH );
    s->size = size;
    s->crc32 = s->rx.u.up_req.crc32;
    s->chunk_size = cs;
    s->total = total;
    memset( s->bitmap, 0, bitmap_bytes( total ) );

    if ( staged ) {
        if ( total == 0u ) {
            st = s->cfg.fs->truncate( s->cfg.fs->ctx, s->path ); /* an empty file is complete right away */
        }
    } else if ( !vftp_sidecar_path( s->path, "VFM", true, s->map_path, sizeof( s->map_path ) ) ) {
        st = VFTP_INVAL;
    } else if ( map_load( s ) ) {
        VFTP_LOG_INFO( "vftp_burst: upload %s resumes at %lu/%lu chunks", s->path, (unsigned long) s->complete,
                       (unsigned long) total );
    } else {
        memset( s->bitmap, 0, bitmap_bytes( total ) );
        st = s->cfg.fs->truncate( s->cfg.fs->ctx, s->path );
        if ( st == VFTP_OK ) {
            st = map_create( s );
        }
    }
    if ( st != VFTP_OK ) {
        session_reset( s );
        reply_ret( s, VFTP_MSG_UPLOAD_REPLY, st, send, ctx );
        return VFTP_SRV_KEEP;
    }
    VFTP_LOG_INFO( "vftp_burst: upload %s, %lu bytes in %lu chunks (%s)", s->path, (unsigned long) size,
                   (unsigned long) total, staged ? "staged" : "write-behind" );
    vftp_crcjob_init( &s->crc, size ); /* advanced from storage at CRC_REQUEST, once all is written */
    s->session = new_session( s );
    reply_ret( s, VFTP_MSG_UPLOAD_REPLY, VFTP_OK, send, ctx );
    return VFTP_SRV_KEEP;
}

static void on_data( vftp_burst_server_t *s )
{
    const vftp_data_t *d = &s->rx.u.data;
    if ( s->state != ST_UPLOAD || d->chunk >= s->total ||
         d->len != vftp_chunk_len( s->size, s->chunk_size, d->chunk ) || vftp_bit_get( s->bitmap, d->chunk ) ) {
        return; /* stale, out of range, malformed or duplicate: drop silently */
    }
    s->activity++;
    uint32_t cs = s->chunk_size;
    if ( s->staged ) {
        memcpy( s->buf + d->chunk * cs, d->bytes, d->len );
        vftp_bit_set( s->bitmap, d->chunk );
        s->complete++;
        if ( s->complete == s->total ) {
            const vftp_storage_t *fs = s->cfg.fs;
            vftp_ret_t st = fs->truncate( fs->ctx, s->path );
            if ( st == VFTP_OK ) {
                st = vftp_fs_write_all( fs, s->path, 0, s->buf, s->size );
            }
            if ( st != VFTP_OK ) {
                VFTP_LOG_WARN( "vftp_burst: staged write of %s failed (%d)", s->path, (int) st );
                memset( s->bitmap, 0, bitmap_bytes( s->total ) ); /* report nothing held: client resends */
                s->complete = 0;
                s->io_err = (uint8_t) st;
                if ( s->io_streak < UINT8_MAX ) {
                    s->io_streak++;
                }
            } else {
                s->io_streak = 0;
            }
        }
        return;
    }
    if ( s->run_count != 0u &&
         ( d->chunk != s->run_first + s->run_count || ( s->run_count + 1u ) * cs > sizeof( s->buf ) ) ) {
        (void) flush_run( s );
    }
    if ( s->run_count == 0u ) {
        s->run_first = d->chunk;
    }
    memcpy( s->buf + s->run_count * cs, d->bytes, d->len );
    s->run_count++;
    vftp_bit_set( s->bitmap, d->chunk );
    s->complete++;
    if ( ( s->run_count + 1u ) * cs > sizeof( s->buf ) || s->complete == s->total ) {
        (void) flush_run( s );
    }
}

static void on_status_request( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    if ( s->state != ST_UPLOAD ) {
        reply_ret( s, VFTP_MSG_STATUS_REPLY, VFTP_INVAL, send, ctx );
        return;
    }
    s->activity++;
    if ( !s->staged ) {
        (void) flush_run( s ); /* once per round: what STATUS reports is durable */
    }
    if ( s->io_streak >= VFTP_CFG_IO_FAIL_LIMIT ) {
        /* The card keeps refusing: resending would never end. The session stays open (and the
         * sidecar keeps what is durable), so a later attempt resumes once storage is fixed. */
        reply_ret( s, VFTP_MSG_STATUS_REPLY, (vftp_ret_t) s->io_err, send, ctx );
        return;
    }
    uint32_t max = VFTP_STATUS_RANGES_FOR( (uint32_t) s->cfg.mtu_tx );
    if ( max > VFTP_WIRE_MAX_RANGES ) {
        max = VFTP_WIRE_MAX_RANGES;
    }
    s->tx.type = VFTP_MSG_STATUS_REPLY;
    s->tx.session = s->session;
    s->tx.u.status.ret = VFTP_OK;
    s->tx.u.status.total = s->total;
    s->tx.u.status.complete = s->complete;
    s->tx.u.status.missing.entries =
        vftp_bitmap_missing( s->bitmap, s->total, s->tx.u.status.missing.range, (uint16_t) max, 0u );
    (void) send_tx( s, send, ctx );
}

static void on_crc_request( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    vftp_ret_t st = VFTP_OK;
    if ( s->state != ST_IDLE ) {
        s->activity++;
    }
    if ( s->state == ST_UPLOAD ) {
        if ( !s->staged ) {
            st = flush_run( s );
        }
        if ( st == VFTP_OK && s->complete < s->total ) {
            st = VFTP_RANGE; /* not everything arrived yet */
        }
    } else if ( s->state != ST_DOWNLOAD ) {
        st = VFTP_INVAL;
    }
    if ( st == VFTP_OK ) {
        s->cache_len = 0; /* buf is the CRC scratch now */
        st = vftp_crcjob_step( &s->crc, s->cfg.fs, s->path, s->buf, (uint32_t) sizeof( s->buf ),
                               s->crc_step );
    }
    if ( st == VFTP_OK && !s->crc.done ) {
        st = VFTP_AGAIN;
    }
    s->tx.type = VFTP_MSG_CRC_REPLY;
    s->tx.session = s->session;
    if ( st == VFTP_INVAL ) {
        s->tx.session = s->rx.session; /* nothing open: echo the asker's session */
    }
    s->tx.u.crc_rep.ret = (uint8_t) st;
    s->tx.u.crc_rep.crc32 = st == VFTP_OK ? s->crc.value : 0u;
    s->tx.u.crc_rep.done = s->crc.off;
    (void) send_tx( s, send, ctx );
}

static void finish( vftp_burst_server_t *s )
{
    if ( s->state == ST_UPLOAD && !s->staged ) {
        (void) flush_run( s );
        if ( s->complete == s->total ) {
            (void) s->cfg.fs->remove( s->cfg.fs->ctx, s->map_path ); /* done for good */
        }
    }
    if ( s->state != ST_IDLE ) {
        VFTP_LOG_INFO( "vftp_burst: %s %s closed at %lu/%lu chunks", s->state == ST_UPLOAD ? "upload" : "download",
                       s->path, (unsigned long) ( s->state == ST_UPLOAD ? s->complete : s->total ),
                       (unsigned long) s->total );
    }
    session_reset( s );
}

static void remove_sidecars( vftp_burst_server_t *s, const char *path )
{
    char side[VFTP_SIDECAR_PATH_MAX];
    if ( vftp_sidecar_path( path, "VFM", true, side, sizeof( side ) ) ) {
        (void) s->cfg.fs->remove( s->cfg.fs->ctx, side );
    }
    if ( vftp_sidecar_path( path, "VFI", true, side, sizeof( side ) ) ) {
        (void) s->cfg.fs->remove( s->cfg.fs->ctx, side );
    }
}

static vftp_srv_action_t on_delete( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    const char *path = s->rx.u.path_req.path;
    vftp_ret_t st;
    if ( s->state != ST_IDLE && vftp_str_eq( path, s->path ) ) {
        st = VFTP_BUSY;
    } else if ( ( st = vftp_client_path_ok( s->cfg.fs, path ) ) == VFTP_OK ) {
        st = s->cfg.fs->remove( s->cfg.fs->ctx, path );
        if ( st == VFTP_OK ) {
            remove_sidecars( s, path );
        }
    }
    s->list_valid = false;
    reply_ret( s, VFTP_MSG_DELETE_REPLY, st, send, ctx );
    return s->state == ST_IDLE ? VFTP_SRV_CLOSE : VFTP_SRV_KEEP;
}

/* Packs entries in walk order while they fit the packet; the first one that does not fit closes the
 * page (later, shorter names are not slipped in: a page is always a contiguous run of indices). The
 * walk itself goes on, so page 0 still counts the whole directory. */
static bool list_fill( void *arg, uint32_t index, const vftp_dirent_t *e )
{
    vftp_burst_server_t *s = (vftp_burst_server_t *) arg;
    (void) index;
    size_t n = vftp_str_len( e->name, VFTP_MAX_FILE_NAME );
    uint32_t need = VFTP_LIST_ENTRY_FIXED + (uint32_t) n;
    if ( s->list_full || s->tx.u.list_rep.packet_files >= VFTP_WIRE_MAX_ENTRIES ||
         s->list_bytes + need > s->cfg.mtu_tx ) {
        s->list_full = true;
        return true;
    }
    vftp_list_entry_t *out = &s->tx.u.list_rep.entry[s->tx.u.list_rep.packet_files++];
    memcpy( out->name, e->name, n );
    out->name[n] = '\0';
    out->flags = (uint8_t) ( ( e->is_dir ? VFTP_ENTRY_DIR : 0u ) | ( e->name_cut ? VFTP_ENTRY_NAME_CUT : 0u ) );
    out->size = e->size;
    out->mtime = e->mtime;
    s->list_bytes = (uint16_t) ( s->list_bytes + need );
    return true;
}

static vftp_srv_action_t on_list( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    const char *dir = s->rx.u.list_req.path;
    uint32_t offset = s->rx.u.list_req.offset;
    uint32_t limit = s->rx.u.list_req.limit;
    if ( limit == 0u || limit > VFTP_WIRE_MAX_ENTRIES ) {
        limit = VFTP_WIRE_MAX_ENTRIES; /* the packet size cuts the page shorter where names are long */
    }
    /* Page 0 counts the whole directory in the same pass; later pages of the same listing reuse
     * that total and stop right after their own entries. */
    bool known = offset != 0u && s->list_valid && vftp_str_eq( dir, s->list_dir );
    s->tx.type = VFTP_MSG_LIST_REPLY;
    s->tx.session = 0u;
    s->tx.u.list_rep.packet_files = 0;
    s->list_bytes = VFTP_LIST_HEADER;
    s->list_full = false;
    uint32_t total = known ? s->list_total : 0u;
    vftp_ret_t st = vftp_dir_scan( s->cfg.fs, dir, offset, limit, list_fill, s, &total, known );
    if ( !known ) {
        memcpy( s->list_dir, dir, VFTP_MAX_PATH );
        s->list_total = total;
        s->list_valid = st == VFTP_OK;
    }
    s->tx.u.list_rep.ret = (uint8_t) ( ( st != VFTP_OK && total == 0u ) ? st : VFTP_OK );
    s->tx.u.list_rep.total_files = total;
    s->tx.u.list_rep.first_file = offset;
    (void) send_tx( s, send, ctx );
    /* The last page ends the conversation right here: holding it open until the idle timeout is
     * what made back-to-back `ls` slow. A lost last page is just asked again (a new conversation
     * on a connectionless link; a reliable link does not lose it). */
    bool last = s->tx.u.list_rep.ret != VFTP_OK || offset + s->tx.u.list_rep.packet_files >= total;
    return ( last && s->state == ST_IDLE ) ? VFTP_SRV_CLOSE : VFTP_SRV_KEEP;
}

#define LIST_TMP "LISTTMP.TXT" /* LIST.TXT is built here, then renamed: never seen half-written */

/* "", "/", ".", "./", "//./" ...: the directory LIST.TXT itself lives in. */
static bool is_root_dir( const char *dir )
{
    for ( ; *dir != '\0'; dir++ ) {
        if ( *dir != '/' && !( *dir == '.' && ( dir[1] == '/' || dir[1] == '\0' ) ) ) {
            return false;
        }
    }
    return true;
}

/* "YYYY-MM-DD_HH:MM" (UTC) of a seconds-since-1970 mtime, "-" when unknown. */
static void list_date( uint32_t t, char *out, size_t cap )
{
    if ( t == 0u ) {
        snprintf( out, cap, "-" );
        return;
    }
    vftp_civil_t c;
    vftp_unix_to_civil( t, &c );
    snprintf( out, cap, "%04u-%02u-%02u_%02u:%02u", (unsigned int) c.year, (unsigned int) c.month,
              (unsigned int) c.day, (unsigned int) c.hour, (unsigned int) c.minute );
}

typedef struct {
    vftp_burst_server_t *s;
    uint32_t used;  /* bytes of LIST.TXT text in s->buf */
    uint32_t next;  /* first entry index not yet in buf */
    uint32_t lines; /* entries written */
    bool root;      /* listing the root: LIST.TXT and its temporary are not entries */
} regen_t;

static bool regen_line( void *arg, uint32_t index, const vftp_dirent_t *e )
{
    regen_t *g = (regen_t *) arg;
    if ( g->root && !e->is_dir && ( vftp_str_ieq( e->name, VFTP_LIST_TXT ) || vftp_str_ieq( e->name, LIST_TMP ) ) ) {
        g->next = index + 1u;
        return true;
    }
    char date[32]; /* "YYYY-MM-DD_HH:MM" with room the compiler can prove */
    char line[64];
    list_date( e->mtime, date, sizeof( date ) );
    /* NAME/ = directory, NAME~ = only the start of a longer name (as csp-client's ls prints them) */
    int n = snprintf( line, sizeof( line ), "%10lu %s %s%s%s\n", (unsigned long) e->size, date, e->name,
                      e->is_dir ? "/" : "", e->name_cut ? "~" : "" );
    if ( n <= 0 || (size_t) n >= sizeof( line ) ) {
        g->next = index + 1u;
        return true;
    }
    if ( g->used + (uint32_t) n > sizeof( g->s->buf ) ) {
        return false; /* buffer full: write it out, then walk on from `next` */
    }
    memcpy( g->s->buf + g->used, line, (size_t) n );
    g->used += (uint32_t) n;
    g->next = index + 1u;
    g->lines++;
    return true;
}

/* Listing method 1: "LIST.TXT" in the root, whatever directory is listed --
 *   # <dir>
 *   <size> <date> <name>[/]      one line per entry, `ls` order
 *   # <n> entries                 last line: the file is complete
 * Built in LISTTMP.TXT and renamed over LIST.TXT when the port can rename, so a download never
 * catches it half-written and a failed regen leaves the previous list intact. */
static vftp_srv_action_t on_list_regen( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    if ( s->state != ST_IDLE ) { /* buf is in use by the open transfer */
        reply_ret( s, VFTP_MSG_LIST_REGEN_REPLY, VFTP_BUSY, send, ctx );
        return VFTP_SRV_KEEP;
    }
    const vftp_storage_t *fs = s->cfg.fs;
    const char *dir = s->rx.u.path_req.path;
    const char *out = fs->rename != NULL ? LIST_TMP : VFTP_LIST_TXT;
    uint32_t total = 0;
    vftp_ret_t st = vftp_dir_scan( fs, dir, 0u, 0u, NULL, NULL, &total, false );
    if ( st == VFTP_NOENT && fs->walk == NULL ) {
        st = VFTP_OK; /* dirent() cannot tell an empty directory from a missing one: list it as empty */
    }
    if ( st == VFTP_OK ) {
        st = fs->truncate( fs->ctx, out ); /* created before the walk, so it never shifts the indices */
    }
    regen_t g = { s, 0u, 0u, 0u, is_root_dir( dir ) };
    uint32_t file_off = 0;
    if ( st == VFTP_OK ) {
        int n = snprintf( (char *) s->buf, sizeof( s->buf ), "# %s\n", dir[0] != '\0' ? dir : "/" );
        st = vftp_fs_write_all( fs, out, 0u, s->buf, (uint32_t) n );
        file_off = (uint32_t) n;
    }
    /* Text is collected in RAM and written between walks: a storage write must never happen
     * inside a walk callback (the port may be holding its filesystem lock). */
    while ( st == VFTP_OK ) {
        g.used = 0;
        uint32_t start = g.next;
        uint32_t dummy = 0;
        st = vftp_dir_scan( fs, dir, start, UINT32_MAX - start, regen_line, &g, &dummy, false );
        if ( st == VFTP_NOENT && start == 0u ) {
            st = VFTP_OK;
        }
        if ( st == VFTP_OK && g.used != 0u ) {
            st = vftp_fs_write_all( fs, out, file_off, s->buf, g.used );
            file_off += g.used;
        }
        if ( g.next == start || g.used == 0u ) {
            break; /* no progress: the walk reached the end */
        }
    }
    if ( st == VFTP_OK ) {
        int n = snprintf( (char *) s->buf, sizeof( s->buf ), "# %lu entries\n", (unsigned long) g.lines );
        st = vftp_fs_write_all( fs, out, file_off, s->buf, (uint32_t) n );
    }
    if ( fs->rename != NULL ) {
        if ( st == VFTP_OK ) {
            (void) fs->remove( fs->ctx, VFTP_LIST_TXT ); /* FAT rename does not replace */
            st = fs->rename( fs->ctx, LIST_TMP, VFTP_LIST_TXT );
        }
        if ( st != VFTP_OK ) {
            (void) fs->remove( fs->ctx, LIST_TMP );
        }
    }
    VFTP_LOG_INFO( "vftp_burst: list of %s -> " VFTP_LIST_TXT ", %lu entries (%d)", dir, (unsigned long) g.lines,
                   (int) st );
    s->list_valid = false;
    reply_ret( s, VFTP_MSG_LIST_REGEN_REPLY, st, send, ctx );
    return VFTP_SRV_CLOSE;
}

static vftp_srv_action_t on_index( vftp_burst_server_t *s, vftp_send_fn send, void *ctx )
{
    vftp_index_req_t q = s->rx.u.idx_req;
    s->tx.type = VFTP_MSG_INDEX_REPLY;
    s->tx.session = 0u;
    vftp_index_lookup( s->cfg.fs, &q, &s->tx.u.idx_rep );
    (void) send_tx( s, send, ctx );
    return s->state == ST_IDLE ? VFTP_SRV_CLOSE : VFTP_SRV_KEEP;
}

/* ---- public -------------------------------------------------------------- */

vftp_ret_t vftp_burst_server_init( vftp_burst_server_t *s, const vftp_server_cfg_t *cfg )
{
    if ( s == NULL || cfg == NULL || cfg->fs == NULL || cfg->mtu_tx > VFTP_CFG_PACKET_MAX ||
         cfg->mtu_rx > VFTP_CFG_PACKET_MAX ||
         cfg->mtu_tx < VFTP_LIST_HEADER + VFTP_LIST_ENTRY_FIXED + VFTP_MAX_FILE_NAME ||
         cfg->mtu_rx <= VFTP_DATA_HEADER ) {
        return VFTP_INVAL;
    }
    s->cfg = *cfg;
    s->crc_step = VFTP_CFG_CRC_STEP_BYTES;
    s->last_session = 0u;
    s->list_valid = false;
    session_reset( s );
    return VFTP_OK;
}

vftp_srv_action_t vftp_burst_server_handle( vftp_burst_server_t *s, const uint8_t *pkt, uint16_t len,
                                            vftp_send_fn send, void *send_ctx )
{
    if ( vftp_decode( pkt, len, &s->rx ) != VFTP_OK ) {
        return VFTP_SRV_KEEP; /* malformed: ignore, never let one bad packet tear down a transfer */
    }
    if ( s->state != ST_IDLE && vftp_msg_has_session( s->rx.type ) && s->rx.session != s->session ) {
        /* A late packet of an earlier transfer (or of another client): dropped before it can touch
         * this one -- no write, no reply, no activity. The client of the open session is unaffected;
         * a stale client times out and starts a new session (resuming from the sidecar). */
        return VFTP_SRV_KEEP;
    }
    switch ( s->rx.type ) {
        case VFTP_MSG_DOWNLOAD_REQUEST:
            return on_download_request( s, send, send_ctx );
        case VFTP_MSG_WANT:
            return on_want_ranges( s, send, send_ctx );
        case VFTP_MSG_UPLOAD_REQUEST:
            return on_upload_request( s, send, send_ctx );
        case VFTP_MSG_DATA:
            on_data( s );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_STATUS_REQUEST:
            on_status_request( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_CRC_REQUEST:
            on_crc_request( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_DONE:
        case VFTP_MSG_ABORT:
            finish( s );
            return VFTP_SRV_CLOSE;
        case VFTP_MSG_DELETE_REQUEST:
            return on_delete( s, send, send_ctx );
        case VFTP_MSG_LIST_REQUEST:
            return on_list( s, send, send_ctx );
        case VFTP_MSG_LIST_REGEN_REQUEST:
            return on_list_regen( s, send, send_ctx );
        case VFTP_MSG_INDEX_REQUEST:
            return on_index( s, send, send_ctx );
        default: /* replies (only this side sends them) and poll-only types */
            return VFTP_SRV_KEEP;
    }
}

void vftp_burst_server_abort( vftp_burst_server_t *s )
{
    finish( s ); /* flushes write-behind data, keeps the sidecar of an incomplete upload */
    s->list_valid = false;
}

bool vftp_burst_server_busy( const vftp_burst_server_t *s )
{
    return s->state != ST_IDLE;
}
